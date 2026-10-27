#include "component_manager.hpp"

int main(int argc, char ** argv)
{
  return flux::run_component_container<capa::ComponentManager>(argc, argv);
}
