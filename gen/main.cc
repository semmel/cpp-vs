#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <ctemplate/template.h>
#include <yaml-cpp/yaml.h>

#include "string_util.hh"

namespace fs = boost::filesystem;

/** Config for generating the site, read in from config.yaml */
struct Config {
    std::vector<std::string> files_to_template;
    std::map<std::string, std::string> template_variables;

    std::string versus_file;
    std::string versus_folder;
    std::map<std::string, std::string> versus;

    static Config parse(fs::path);
};

Config Config::parse(fs::path config_path) {
    auto yaml_conf = YAML::LoadFile(config_path.string());
    auto conf = Config {
        .files_to_template = yaml_conf["files"].as<std::vector<std::string>>(),
        .template_variables = {},
        .versus_file   = yaml_conf["versus_file"].as<std::string>(),
        .versus_folder = yaml_conf["versus_folder"].as<std::string>(),
        .versus = {},
    };
    auto vs = yaml_conf["variables"];
    if (vs.IsDefined() && vs.IsMap()) {
        for (auto it = vs.begin(); it != vs.end(); ++it) {
            conf.template_variables[it->first.as<std::string>()] = it->second.as<std::string>();;
        }
    }
    auto versus = yaml_conf["versus"];
    if (versus.IsDefined() && versus.IsSequence()) {
        for (size_t i = 0; i < versus.size(); i++) {
            conf.versus[versus[i]["name"].as<std::string>()] = versus[i]["folder"].as<std::string>();
        }
    }
    return conf;
}

/** File representing a page (one that uses an include template) */
struct PageFile {
    std::string layout;
    std::string title;
    std::string body;

    static PageFile parse(fs::path);
};

PageFile PageFile::parse(fs::path file) {
    auto infile = std::ifstream(file.string());
    std::string yaml_input = "";
    std::string line;
    std::stringstream body;
    bool start = false;
    while (std::getline(infile, line)) {
        if (start) {
            yaml_input += line + "\n";
        }
        if (line.find("---") != std::string::npos && line.substr(0, 3) == "---") {
            start = !start;
            continue;
        }
        if (!start) {
            body << line << "\n";
        }
    }
    auto yaml = YAML::Load(yaml_input);
    auto pf = PageFile {
        .layout = yaml["layout"].as<std::string>(),
        .title  = "",
        .body   = body.str(),
    };
    if (yaml["title"]) pf.title = yaml["title"].as<std::string>();
    return pf;
}

std::string read_file(const fs::path &p) {
    std::ifstream t(p.string());
    return std::string((std::istreambuf_iterator<char>(t)),
            std::istreambuf_iterator<char>());
}
std::pair<std::string, size_t> read_file_lines(const fs::path &p) {
    std::ifstream t(p.string());
    return std::pair<std::string, size_t> {
        read_file(p),
        std::count(std::istreambuf_iterator<char>(t), std::istreambuf_iterator<char>(), '\n'),
    };
}
std::string file_ext_to_prism_language(const fs::path &p) {
    auto file = p.filename().string();
    auto n = file.rfind('.');
    if (n != std::string::npos) {
        auto ext = file.substr(n + 1, file.length());
        if (ext == "cc") return "cpp";
        if (ext == "js") return "javascript";
    }
    return "none";
}

/** Most of the magic happens here... enjoy */
int main(int argc, char** argv) {
    if (argc <= 1) {
        std::cout << "[Error]: Must specify a site directory\n";
        return 1;
    }
    auto site_dir = fs::path(std::string(argv[1]));
    auto build_dir = (site_dir / ".." / "build" / "site");

    auto config = Config::parse(site_dir / "config.yaml");

    ctemplate::TemplateDictionary template_dict("template_dict");
    for (auto const &pair: config.template_variables) {
        template_dict.SetValue(pair.first, pair.second);
    }
    std::map<std::string, std::string> layouts;

    // Read includes into our template dictionary AND template cache (depending on how they get used)
    for (auto const &dir_entry: fs::recursive_directory_iterator(site_dir / "includes")) {
        if (fs::is_regular_file(dir_entry)) {
            auto filename = std::string(dir_entry.path().filename().c_str());
            auto template_name = filename.substr(0, filename.find(".tpl.html"));

            auto file_str = read_file(dir_entry.path());
            template_dict.SetValue("includes__" + template_name, file_str);
            ctemplate::StringToTemplateCache("includes_" + template_name, file_str, ctemplate::DO_NOT_STRIP);
        }
    }

    // Read layout files into a map
    for (auto const &dir_entry: fs::recursive_directory_iterator(site_dir / "layouts")) {
        if (fs::is_regular_file(dir_entry)) {
            auto filename = dir_entry.path().filename().string();
            std::string template_name = filename.substr(0, filename.find(".tpl.html"));
            layouts[template_name] = read_file(dir_entry.path());
        }
    }

    // lambda for rendering files (used for simple files and versus files)
    auto render_file_with_layout = [&](std::string const &f) -> std::string {
        auto file = site_dir / f;
        auto header = PageFile::parse(file);

        if (layouts.find(header.layout) == layouts.end()) {
            std::cerr << "Can not render to layout '" << header.layout << "' as it does not exist\n";
            std::exit(1);
        }
        // render the page
        std::cout << "Rendering page: " << f << "\n";
        ctemplate::StringToTemplateCache("template_file", layouts[header.layout], ctemplate::DO_NOT_STRIP);
        template_dict.SetValue("content", header.body);
        template_dict.SetValue("title", header.title);
        std::string rendered_template;
        ctemplate::ExpandTemplate("template_file", ctemplate::DO_NOT_STRIP, &template_dict, &rendered_template);

        // second pass to set dynamic data within includes
        ctemplate::StringToTemplateCache("temp", rendered_template, ctemplate::DO_NOT_STRIP);
        rendered_template.clear();
        ctemplate::ExpandTemplate("temp", ctemplate::DO_NOT_STRIP, &template_dict, &rendered_template);
        ctemplate::mutable_default_template_cache()->ClearCache();

        return rendered_template;
    };

    // Render all of the "versus" files
    for (auto const &pair: config.versus) {
        std::cout << "Generating " << pair.first << " versus\n";
        auto base_folder = site_dir / ".." / config.versus_folder / pair.second;
        for (auto const &dir_entry: fs::recursive_directory_iterator(base_folder)) {
            if (dir_entry.path().filename().string() == "versus.yaml") {
                ctemplate::TemplateDictionary dict("template_dict");
                auto vs_conf = YAML::LoadFile(dir_entry.path().string());
                std::cout << "\tAdding versus: " << vs_conf["name"].as<std::string>() << "\n";

                dict.SetValue("name", vs_conf["versus"]["name"].as<std::string>());
                auto render_code_file = [&](std::string &&key) -> std::string {
                    std::string out;
                    for (size_t i = 0; i < vs_conf[key].size(); i++) {
                        ctemplate::TemplateDictionary block_dict("template_dict");
                        auto file_path = dir_entry.path().parent_path() / vs_conf[key][i].as<std::string>();
                        auto file = read_file(file_path);
                        block_dict.SetValue("code", file);
                        block_dict.SetValue("prism_language", file_ext_to_prism_language(file_path));
                        if (vs_conf[key].size() > 1) block_dict.SetValue("file_name", file_path.filename().string());
                        ctemplate::ExpandTemplate("includes_versus_block", ctemplate::DO_NOT_STRIP, &block_dict, &out);
                    }
                    return out;
                };
                dict.SetValue("cpp_code", render_code_file("cpp"));
                    // read_file((dir_entry.path().parent_path() / vs_conf["cpp"][0].as<std::string>()).string()));
                dict.SetValue("other_code", render_code_file("other"));
                    // read_file((dir_entry.path().parent_path() / vs_conf["other"][0].as<std::string>()).string()));

                std::string rendered;
                ctemplate::ExpandTemplate("includes_versus", ctemplate::DO_NOT_STRIP, &dict, &rendered);
                template_dict.SetValue("versus_content", rendered);

                std::ofstream versus_file;
                versus_file.open((build_dir / "versus" / vs_conf["filename"].as<std::string>()).string());
                versus_file << render_file_with_layout(config.versus_file);
                versus_file.close();
            }
        }
    }

    // Render all of the basic files
    for (auto const &f: config.files_to_template) {
        // write out rendered page to disk
        std::ofstream rendered_file;
        rendered_file.open((build_dir / str_replace(f, ".tpl.html", ".html")).string());
        rendered_file << render_file_with_layout(f);
        rendered_file.close();
    }
    return 0;
}