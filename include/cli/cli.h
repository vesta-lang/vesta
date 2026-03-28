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
#ifndef CLI_H
#define CLI_H

#include <functional>  // for function
#include <iostream>  // for basic_ostream::operator<<, operator<<, endl, basic_ostream, basic_ostream<>::__ostream_type, cout, ostream
#include <string>    // for string, basic_string, allocator
#include <vector>    // for vector

#include "ftxui/component/component.hpp"
#include "ftxui/component/loop.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"  /* graph, operator|, separator,
    color, Element, vbox, flex, inverted, operator|=, Fit, hbox, size,
    border, GREATER_THAN, HEIGHT
*/
#include "ftxui/screen/screen.hpp"  // for Full, Screen
#include "ftxui/dom/node.hpp"  // for Render
#include "ftxui/screen/color.hpp"  // for Color, Color::BlueLight, Color::RedLight, Color::YellowLight, ftxui
#include "ftxui/component/component.hpp"                // for App
#include "ftxui/component/captured_mouse.hpp"     // for ftxui
#include "ftxui/component/component_options.hpp"  // for MenuOption

#include "runtime/manager_runtime.h"
#include "runtime/runtime.h"


namespace cli {

    class VestaViewManager {
        runtime::ManageVM manager;
        size_t            selected_vm = 0;
        ftxui::Component  screen;
        std::string       content;

        // Estado del dropdown de pantalla de instancias
        bool open = false;
        int selected_option = 0;

    public:
        void run();

        ftxui::Component crearPantallaInstancias(ftxui::Component input);

        ftxui::Component view();

        VestaViewManager(const runtime::ManageVM& manager);

    private:
        int selectedEntry = 0;
        std::vector<std::string> entries = {
            "Ver instancias",
            "Memoria global",
            "Metadatos global",
        };

        std::vector<std::string> nombresInstancias = {};
        std::string placeholder = "cmd>";

    };

}

#endif