#ifndef CAPAD_BRIDGE_HPP
#define CAPAD_BRIDGE_HPP

#include "capa/spec.hpp"
#include "rust/cxx.h"

#include <cstdint>
#include <memory>

// The C++ half of src/ffi.rs. Each function is one capa_core call with its types converted; the
// declarations and their meaning are in ffi.rs.

namespace capad::bridge
{

struct Stage;
struct ThreadState;
struct Binding;
struct HeldStage;

auto load(rust::Str path) -> std::unique_ptr<capa::Spec>;
auto stages(const capa::Spec & spec) -> rust::Vec<Stage>;
auto refusal(const capa::Spec & spec, std::size_t index, int tid) -> rust::String;
auto drift(const capa::Spec & spec, std::size_t index, int tid) -> rust::String;
void apply(const capa::Spec & spec, std::size_t index, int tid);
auto observe(int tid) -> ThreadState;
void put_back(int tid, const ThreadState & state);
void move_to(int tid, rust::Slice<const std::uint32_t> cpus);
auto bindings() -> rust::Vec<Binding>;
auto held_stages() -> rust::Vec<HeldStage>;
auto daemon_dir() -> rust::String;
auto record(rust::Slice<const Binding> held) -> rust::String;
auto task_starttime(int pid, int tid) -> std::uint64_t;
void bind(rust::Str target);
auto this_tid() -> int;

}  // namespace capad::bridge

#endif  // CAPAD_BRIDGE_HPP
