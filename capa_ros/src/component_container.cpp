#include "component_manager.hpp"

auto main(int argc, char ** argv) -> int
{
  return flux::run_component_container<capa::ComponentManager>(argc, argv);
}
