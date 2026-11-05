#include "../../capa_core/test/result_test_util.hpp"
#include "capa/flux.hpp"
#include "component_manager.hpp"

#include <composition_interfaces/srv/load_node.hpp>
#include <rclcpp/rclcpp.hpp>

#include <gtest/gtest.h>
#include <rcutils/logging.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using composition_interfaces::srv::LoadNode;

template <class P>
bool eventually(P done, std::chrono::milliseconds limit = 10000ms)
{
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > end) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

std::mutex log_m;
std::vector<std::string> log_lines;
rcutils_logging_output_handler_t ros_handler = nullptr;

void capture(
  const rcutils_log_location_t * where, int severity, const char * name,
  rcutils_time_point_value_t stamp, const char * format, va_list * args)
{
  va_list copy;
  va_copy(copy, *args);
  char line[1024];
  std::vsnprintf(line, sizeof(line), format, copy);
  va_end(copy);
  {
    const std::lock_guard<std::mutex> lk(log_m);
    log_lines.emplace_back(line);
  }
  ros_handler(where, severity, name, stamp, format, args);
}

std::vector<std::string> logged(const std::string & part)
{
  const std::lock_guard<std::mutex> lk(log_m);
  std::vector<std::string> out;
  for (const auto & l : log_lines) {
    if (l.find(part) != std::string::npos) {
      out.push_back(l);
    }
  }
  return out;
}

// What each group thread of the planner ran with once the manager's hook returned, by the test
// topic the group holds.
class Manager : public capa::ComponentManager
{
public:
  explicit Manager(const std::string & name) : capa::ComponentManager(name) {}

  std::vector<ComponentResource> get_component_resources(
    const std::string & package, const std::string &) const override
  {
    if (package != "capa_ros_test") {
      return {};
    }
    return {{"capa_ros_test::Planner", CAPA_TEST_COMPONENTS}};
  }

  std::map<std::string, std::vector<std::uint32_t>> started()
  {
    const std::lock_guard<std::mutex> lk(m_);
    return started_;
  }

protected:
  void on_thread_start(
    const NodeBase::SharedPtr & node, const rclcpp::CallbackGroup::SharedPtr & group) override
  {
    capa::ComponentManager::on_thread_start(node, group);
    if (std::string(node->get_name()) != "planner") {
      return;
    }
    const auto subs = capa::flux::content(*node, group).subscriptions;
    std::string key;
    for (const char * t : {"/capa_ct/odom", "/capa_ct/traj", "/capa_ct/map", "/capa_ct/frames"}) {
      if (std::find(subs.begin(), subs.end(), t) != subs.end()) {
        key = t;
      }
    }
    const std::lock_guard<std::mutex> lk(m_);
    started_[key] = capa::current().cpus;
  }

private:
  std::mutex m_;
  std::map<std::string, std::vector<std::uint32_t>> started_;
};

class Container : public ::testing::Test
{
protected:
  void start(const std::string & spec)
  {
    path_ = "/tmp/capa_container_" + std::to_string(::getpid()) + ".yaml";
    std::ofstream(path_) << spec;
    ::setenv("CAPA_SPEC", path_.c_str(), 1);
    rclcpp::init(0, nullptr);
    ros_handler = rcutils_logging_get_output_handler();
    rcutils_logging_set_output_handler(capture);
    const std::string name = "capa_container_" + std::to_string(::getpid());
    manager_ = std::make_shared<Manager>(name);
    spinner_ = std::thread([this] {
      try {
        manager_->spin();
      } catch (const std::exception & e) {
        const std::lock_guard<std::mutex> lk(log_m);
        error_ = e.what();
      }
      done_ = true;
    });
    client_ = std::make_shared<rclcpp::Node>(name + "_client");
    load_ = client_->create_client<LoadNode>("/" + name + "/_container/load_node");
    ASSERT_TRUE(load_->wait_for_service(10s));
  }

  void TearDown() override
  {
    if (manager_) {
      manager_->cancel();
      spinner_.join();
      manager_.reset();
    }
    client_.reset();
    load_.reset();
    if (rclcpp::ok()) {
      rclcpp::shutdown();
    }
    rcutils_logging_set_output_handler(ros_handler);
    ::unsetenv("CAPA_SPEC");
    std::remove(path_.c_str());
    const std::lock_guard<std::mutex> lk(log_m);
    log_lines.clear();
  }

  std::shared_future<LoadNode::Response::SharedPtr> load_planner()
  {
    auto request = std::make_shared<LoadNode::Request>();
    request->package_name = "capa_ros_test";
    request->plugin_name = "capa_ros_test::Planner";
    return load_->async_send_request(request).future.share();
  }

  LoadNode::Response::SharedPtr wait(const std::shared_future<LoadNode::Response::SharedPtr> & f)
  {
    rclcpp::executors::SingleThreadedExecutor ex;
    ex.add_node(client_);
    EXPECT_EQ(ex.spin_until_future_complete(f, 10s), rclcpp::FutureReturnCode::SUCCESS);
    return f.get();
  }

  std::string error()
  {
    const std::lock_guard<std::mutex> lk(log_m);
    return error_;
  }

  std::string path_;
  std::shared_ptr<Manager> manager_;
  std::thread spinner_;
  std::atomic<bool> done_{false};
  std::string error_;
  rclcpp::Node::SharedPtr client_;
  rclcpp::Client<LoadNode>::SharedPtr load_;
};

std::string cpu(std::uint32_t c)
{
  return "[" + std::to_string(c) + "]";
}

}  // namespace

// The default group and three groups the code never names to capa: one picked by the rclcpp
// subscription it holds, one by the flux subscription it holds, and one the file does not declare.
// The two stages that name no group of the node are reported at load, one line each.
TEST_F(Container, BindsTheDeclaredGroupsAndWarnsOfStagesThatBindNothing)
{
  const auto all = capa::current().cpus;
  if (all.size() < 5) {
    GTEST_SKIP() << "needs five cpus to tell the stages apart";
  }
  start(
    "chains:\n"
    "  c:\n"
    "    target: soft\n"
    "    stages:\n"
    "      - {node: /planner, cpus: " +
    cpu(all[0]) +
    "}\n"
    "      - {node: /planner, group: traj, holds: {subscription: /capa_ct/traj}, cpus: " +
    cpu(all[1]) +
    "}\n"
    "      - {node: /planner, group: frames, holds: {subscription: /capa_ct/frames}, cpus: " +
    cpu(all[2]) +
    "}\n"
    "      - {node: /planner, group: stale, holds: {subscription: /capa_ct/trajectory}, cpus: " +
    cpu(all[3]) +
    "}\n"
    "      - {node: /planner, group: work, cpus: " +
    cpu(all[4]) + "}\n");

  ASSERT_TRUE(wait(load_planner())->success);
  ASSERT_TRUE(eventually([&] { return manager_->started().size() == 4; }));
  const auto started = manager_->started();
  EXPECT_EQ(started.at("/capa_ct/odom"), std::vector<std::uint32_t>{all[0]});
  EXPECT_EQ(started.at("/capa_ct/traj"), std::vector<std::uint32_t>{all[1]});
  EXPECT_EQ(started.at("/capa_ct/frames"), std::vector<std::uint32_t>{all[2]});
  EXPECT_EQ(started.at("/capa_ct/map"), all);

  EXPECT_EQ(logged("bound to thread").size(), 3U);
  const auto unbound = logged("binds no thread");
  ASSERT_EQ(unbound.size(), 2U);
  EXPECT_NE(unbound[0].find("/planner/stale"), std::string::npos) << unbound[0];
  EXPECT_NE(unbound[1].find("/planner/work"), std::string::npos) << unbound[1];
}

// A stage the host refuses stops the container with the stage's name, whatever the chain's
// target: a thread does not run at a priority other than the file says.
TEST_F(Container, ARefusedStageStopsTheContainer)
{
  capa::Options fifo;
  fifo.policy = capa::Policy::Fifo;
  fifo.priority = 10;
  if (must(capa::preflight(fifo)).ok()) {
    GTEST_SKIP() << "capa-cap:rtprio this host grants SCHED_FIFO, so nothing refuses the stage";
  }
  start(
    "chains:\n"
    "  c:\n"
    "    target: soft\n"
    "    stages:\n"
    "      - {node: /planner, group: traj, holds: {subscription: /capa_ct/traj}, policy: fifo, "
    "priority: 10}\n");

  load_planner();
  ASSERT_TRUE(eventually([&] { return done_.load(); }));
  EXPECT_NE(error().find("capa: stage /planner/traj"), std::string::npos) << error();
}

TEST_F(Container, TwoStagesOnOneGroupRefuseTheLoad)
{
  start(
    "chains:\n"
    "  c:\n"
    "    target: soft\n"
    "    stages:\n"
    "      - {node: /planner}\n"
    "      - {node: /planner, group: odom, holds: {subscription: /capa_ct/odom}}\n");

  const auto response = wait(load_planner());
  EXPECT_FALSE(response->success);
  EXPECT_NE(response->error_message.find("one group is one thread"), std::string::npos)
    << response->error_message;
}
