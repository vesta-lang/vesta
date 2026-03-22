#include "cli/cli.h"
#include "ftxui/component/screen_interactive.hpp"

int main() {
    runtime::ManageVM manager;
    manager.create_vm();
    manager.create_vm();

    cli::VestaViewManager debugger(manager);
    debugger.run();
}
