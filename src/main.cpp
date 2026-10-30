// Port of main.py: CLI argument handling plus the top level menu loop.
#include <argparse/argparse.hpp>
#include <exception>
#include <iostream>
#include <string>

#include "api.hpp"
#include "console.hpp"
#include "info.hpp"
#include "jsoncli.hpp"
#include "menus.hpp"
#include "version.hpp"

namespace {

int run(int argc, char** argv) {
    argparse::ArgumentParser program("tuftools", "1.0", argparse::default_arguments::help);
    program.add_description("TUF Tools");
    program.add_argument("--proxy").help("http proxy URL");
    program.add_argument("--verbose", "-v").help("Verbose output").default_value(false).implicit_value(true);
    program.add_argument("--dump-json").help("Print response JSON bodies (requires -v)").default_value(false).implicit_value(true);
    program.add_argument("--no-cache").help("Do not read or write the response cache")
        .default_value(false)
        .implicit_value(true);
    program.add_argument("--json", "-j")
        .help("Non-interactive JSON mode: read one JSON request, print one JSON response")
        .default_value(false)
        .implicit_value(true);
    program.add_argument("--request").help("JSON request text (implies --json; \"-\" reads stdin)");
    program.add_argument("--request-file").help("Read the JSON request from this file (implies --json)");
    program.add_argument("--json-indent")
        .help("Pretty-print the JSON response with N spaces (default 0: one compact line)")
        .scan<'i', int>()
        .default_value(0);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << program << std::endl;
        return 2;
    }

    std::string proxy;
    if (const auto value = program.present<std::string>("--proxy")) {
        proxy = *value;
    }
    const bool verbose = program.get<bool>("--verbose");
    const bool dump_json = program.get<bool>("--dump-json");
    const bool no_cache = program.get<bool>("--no-cache");

    if (!proxy.empty()) {
        tuf::set_proxies(proxy);
    }
    tuf::set_verbose(verbose);
    tuf::set_verbose_json(verbose && dump_json);
    tuf::set_cache_enabled(!no_cache);

    tuf::JsonCliOptions json_options;
    const auto request = program.present<std::string>("--request");
    if (request) {
        json_options.request = *request;
    }
    const auto request_file = program.present<std::string>("--request-file");
    if (request_file) {
        json_options.request_file = *request_file;
    }
    if (!json_options.request.empty() && json_options.request != "-" && !json_options.request_file.empty()) {
        std::cerr << "--request 与 --request-file 不能同时使用" << std::endl;
        return 2;
    }
    if (program.get<bool>("--json") || request || request_file) {
        const int indent = program.get<int>("--json-indent");
        json_options.pretty = indent > 0;
        json_options.indent = indent > 0 ? indent : 2;
        // Response documents are the only thing allowed on stdout; the JSON
        // layer flushes the cache itself while stdout is redirected.
        return tuf::run_json_cli(json_options);
    }

    std::cout << "=== TUF Tools ===" << std::endl;
    std::cout << "Github: github.com/sleepingcui/tuftools TUF: tuforums.com" << std::endl;
    std::cout << "版本: " << TUF_VERSION << "   编译时间:" << TUF_BUILD_TIME << std::endl;

    while (true) {
        std::cout << "\n选择功能系统" << std::endl;
        std::cout << "1. 玩家数据查询" << std::endl;
        std::cout << "2. 谱面查询" << std::endl;
        std::cout << "3. PP计算器" << std::endl;
        std::cout << "4. XACC计算器" << std::endl;
        std::cout << "5. 清空缓存" << std::endl;
        std::cout << "q. 退出" << std::endl;

        const std::string choice = tuf::read_trimmed("\n> ");

        try {
            if (choice == "1") {
                tuf::handle_player_lookup();
            } else if (choice == "2") {
                tuf::handle_level_lookup();
            } else if (choice == "3") {
                tuf::handle_pp_calc();
            } else if (choice == "4") {
                tuf::handle_acc_calc();
            } else if (choice == "5") {
                if (no_cache) {
                    std::cout << "已启用 --no-cache，本次运行不会读写缓存" << std::endl;
                } else {
                    std::string error;
                    if (tuf::cache_clear(error)) {
                        std::cout << "缓存已清空" << std::endl;
                    } else {
                        std::cout << "缓存清空失败: " << error << std::endl;
                    }
                }
            } else if (choice == "q") {
                std::cout << "exit" << std::endl;
                break;
            } else {
                std::cout << "无效选择" << std::endl;
            }
        } catch (const tuf::InputClosed&) {
            // Piped input ended: let main() turn it into a normal quit.
            throw;
        } catch (const std::exception& e) {
            std::cout << "错误: " << e.what() << std::endl;
        }
    }

    tuf::cache_flush();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    tuf::setup_console_utf8();
    try {
        return run(argc, argv);
    } catch (const tuf::InputClosed&) {
        // Piped input ended: behave like a normal quit instead of looping.
        tuf::cache_flush();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
}
