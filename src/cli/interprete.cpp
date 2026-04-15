#include "cli/cli.h"

#include <cstdio>

#include "cli/cli_init_manager_and_server.h"
#include "cli/sync_io.h"
#include "emmit/parser_to_bytecode.h"

namespace cli {
    using namespace Assembly::Bytecode;

    VestaInterprete::VestaInterprete() {}

    std::vector<uint8_t> VestaInterprete::run_interprete_execute_code() {
        vm::Lexer  lexer(code);
        vm::Parser parser(lexer);
        // Ensamblar
        Assembler asmblr;
        return asmblr.assemble(parser.parse());
    }

    std::vector<std::string> VestaInterprete::tokenize(const std::string &input) {
        std::vector<std::string> tokens;
        std::string              token;
        std::istringstream       iss(input);

        while (iss >> token) {
            // ignora espacios múltiples automáticamente
            // pasar a minúsculas
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) {
                               return std::tolower(c);
                           });
            tokens.push_back(token);
        }

        return tokens;
    }

    void VestaInterprete::run_interprete() {
        uint8_t             id      = manager.create_vm(1);
        uint64_t            last_ip = 0;
        runtime::VM *       vm      = manager.get_vm(id);
        vm->start();

        GlobalPID           pid     = vm->spawn_process();
        runtime::ProcessVM *process = vm->get_process(pid);

        while (running) {
            try {
                vesta::scout() << "vesta[interpete]{" << (int) id << "}> ";

                if (!std::getline(std::cin, code))
                    break;

                // quitar espacios al inicio y final
                auto trim = [](std::string &s) {
                    s.erase(0, s.find_first_not_of(" \t"));
                    s.erase(s.find_last_not_of(" \t") + 1);
                };
                trim(code);

                auto tokens = tokenize(code);


                if (tokens.empty())
                    continue;

                // comando quit
                if (tokens[0] == "quit") {
                    break;
                }

                // comando bytecode
                if (tokens[0] == "bytecode") {
                    if (tokens.size() == 2 && tokens[1] == "on") {
                        show_bytecode = true;
                        std::cout << "[info] bytecode ON\n";
                        continue;
                    }

                    if (tokens.size() == 2 && tokens[1] == "off") {
                        show_bytecode = false;
                        std::cout << "[info] bytecode OFF\n";
                        continue;
                    }

                    std::cout << "[error] uso: bytecode on | bytecode off\n";
                    continue;
                }

                if (tokens[0] == "status") {
                    std::cout << vm->to_string() << std::endl;
                    continue;
                }

                std::vector<uint8_t> bytecode = run_interprete_execute_code();

                if (show_bytecode)
                    vesta::scout() << vesta::dump(bytecode.data(), bytecode.size()) << std::endl;

                // HLT = 00 03
                bytecode.push_back(0x00);
                bytecode.push_back(0x03);

                process->load_raw_code(last_ip, bytecode);
                //vm->join();
                last_ip = process->registers.rip.raw();
            } catch (const std::exception &e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
        }

        manager.destroy_all_vms();
    }
}
