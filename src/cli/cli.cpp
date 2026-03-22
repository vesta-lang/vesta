/*
* VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

#include "cli/cli.h"

#include <cstdio>

namespace cli {
    using namespace ftxui;

    Component VestaViewManager::crearPantallaInstancias(Component input) {
        return Renderer([&] {
            return vbox({
                text("Instancias"),
                separator()
            });
        });
    }




    Component VestaViewManager::view() {
        using namespace ftxui;

        std::string placeholder = "cmd>";
        auto        input       = Input(&content, &placeholder);

        input |= CatchEvent([&](Event event) {
            if (event.is_character() && content.size() > 10)
                return true;
            return false;
        });

        MenuOption option;
        auto       menu = Menu(&entries, &selectedEntry);

        auto pantallaInstancias = crearPantallaInstancias(input);

        auto placeholder_component = Renderer([] {
            return text("Selecciona una opción");
        });

        auto right_panel_base = Container::Vertical({
            placeholder_component
        });

        auto right_panel = Renderer(right_panel_base, [&] {
            switch (selectedEntry) {
                case 0:
                    return pantallaInstancias->Render();
                case 1:
                    return vbox({
                        text("Pantalla 2: " + entries[selectedEntry]),
                        separator(),
                        text("Aquí irán los logs...")
                    });
                case 2:
                    return vbox({
                        text("Pantalla 3: " + entries[selectedEntry]),
                        separator(),
                        text("Opciones de configuración...")
                    });
                default:
                    return text("Nada seleccionado");
            }
        });

        auto main_layout = Container::Horizontal({
            menu,
            right_panel
        });

        return Renderer(main_layout, [main_layout] {
            return border(
                hbox({
                    main_layout->ChildAt(0)->Render(),
                    separator(),
                    main_layout->ChildAt(1)->Render()
                })
            );
        });
    }



    VestaViewManager::VestaViewManager(const runtime::ManageVM &manager): manager(manager) {
        for (auto &vm: manager.vms) {
            std::stringstream ss;
            ss << "0x" << std::hex << std::uppercase << vm->id.id;
            nombresInstancias.push_back("VM<" + ss.str() + ">");
        }
    }

    void VestaViewManager::run() {
        auto screen = ScreenInteractive::Fullscreen();

        // View principal
        auto main_view = view();

        // CatchEvent para Enter/Quit
        main_view |= CatchEvent([&](ftxui::Event event) {
            if (event == ftxui::Event::Character('q') || event == ftxui::Event::Character('Q')) {
                screen.ExitLoopClosure()();
                return true;
            }
            // ESC para salir
            if (event == ftxui::Event::Escape) {
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        });

        screen.Loop(main_view);
    }
}
