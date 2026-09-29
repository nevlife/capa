#ifndef CYRAD_BRIDGE_HPP
#define CYRAD_BRIDGE_HPP

#include "cyra/spec.hpp"
#include "rust/cxx.h"

#include <cstdint>
#include <memory>

// The C++ half of src/ffi.rs. Each function is one cyra_core call with its types converted; the
// declarations and their meaning are in ffi.rs.

namespace cyrad::bridge
{

struct Stage;
struct ThreadState;
struct Binding;
struct HeldStage;

std::unique_ptr<cyra::Spec> load(rust::Str path);
rust::Vec<Stage> stages(const cyra::Spec & spec);
rust::String refusal(const cyra::Spec & spec, std::size_t index, int tid);
rust::String drift(const cyra::Spec & spec, std::size_t index, int tid);
void apply(const cyra::Spec & spec, std::size_t index, int tid);
ThreadState observe(int tid);
void put_back(int tid, const ThreadState & state);
void move_to(int tid, rust::Slice<const std::uint32_t> cpus);
rust::Vec<Binding> bindings();
rust::Vec<HeldStage> held_stages();
rust::String daemon_dir();
rust::String record(rust::Slice<const Binding> held);
std::uint64_t task_starttime(int pid, int tid);
void bind(rust::Str target);
int this_tid();

}  // namespace cyrad::bridge

#endif  // CYRAD_BRIDGE_HPP
