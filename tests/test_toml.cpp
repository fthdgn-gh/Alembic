// SPDX-License-Identifier: GPL-3.0-or-later
// The TOML subset reader and LZ4 pack profiles read from TOML.
#include "alembic/lz4pack.hpp"
#include "alembic/toml.hpp"
#include "test.hpp"


using namespace alembic;

TEST_CASE(toml_reads_the_profile_subset) {
    auto doc = toml::parse("# comment\n"
                           "title = \"a \\\"q\\\" \\u00e9\" # trailing\n"
                           "lit = 'C:\\path'\n"
                           "[pack]\n"
                           "n = 1_024\n"
                           "neg = -5\n"
                           "hex = 0x1F\n"
                           "f = 0.125\n"
                           "e = 1e3\n"
                           "yes = true\n"
                           "list = [\n  \"a\", # one\n  \"b\",\n]\n"
                           "dotted.key = 3\n"
                           "inline = { x = 1, y = \"z\" }\n"
                           "[groups.assets]\n"
                           "pack_count = 4\n"
                           "[[rule]]\n"
                           "action = \"compress\"\n"
                           "[[rule]]\n"
                           "action = \"loose\"\n");
    REQUIRE_OK(doc);
    CHECK(doc->get("title")->string == "a \"q\" \xC3\xA9");
    CHECK(doc->get("lit")->string == "C:\\path");
    const toml::Value* pack = doc->get("pack");
    CHECK(pack && pack->is_table());
    CHECK_EQ(pack->get("n")->integer, 1024);
    CHECK_EQ(pack->get("neg")->integer, -5);
    CHECK_EQ(pack->get("hex")->integer, 31);
    CHECK(pack->get("f")->floating == 0.125);
    CHECK(pack->get("e")->floating == 1000.0);
    CHECK(pack->get("yes")->boolean);
    CHECK_EQ(pack->get("list")->items.size(), 2u);
    CHECK_EQ(pack->get("dotted")->get("key")->integer, 3);
    CHECK(pack->get("inline")->get("y")->string == "z");
    CHECK_EQ(doc->get("groups")->get("assets")->get("pack_count")->integer, 4);
    const toml::Value* rules = doc->get("rule");
    CHECK(rules && rules->table_array);
    CHECK_EQ(rules->items.size(), 2u);
    CHECK(rules->items[1].get("action")->string == "loose");
}

TEST_CASE(toml_rejects_what_tomllib_rejects) {
    CHECK(!toml::parse("a = 1\na = 2\n").ok());          // duplicate key
    CHECK(!toml::parse("[t]\n[t]\n").ok());              // table defined twice
    CHECK(!toml::parse("a = \"open\n").ok());            // unterminated string
    CHECK(!toml::parse("a = [1, 2\n").ok());             // unterminated array
    CHECK(!toml::parse("a = 1 b = 2\n").ok());           // two values on a line
    CHECK(!toml::parse("a = 012\n").ok());               // leading zero
    CHECK(!toml::parse("a = 1__0\n").ok());              // bad underscore
    CHECK(!toml::parse("a = \"\"\"x\"\"\"\n").ok());     // multi-line string (unsupported)
    CHECK(!toml::parse("d = 2026-10-18\n").ok());        // dates (unsupported)
    CHECK(!toml::parse("a = 1\n[[a]]\n").ok());          // not an array of tables
    CHECK(toml::parse("[a.b]\nx = 1\n[a]\ny = 2\n").ok()); // [a] was only implicit
}

TEST_CASE(lz4_profile_from_toml_follows_load_config) {
    const char* text = "[pack]\n"
                       "default_action = \"loose\"\n"
                       "default_block_size = \"128KiB\"\n"
                       "io_neutral_min_savings_bytes = \"8KiB\"\n"
                       "auto_loose_min_file_size = \"1.5MiB\"\n"
                       "typo_key = 1\n" // ignored, as in ampr_pack.py
                       "\n[groups.assets]\npack_count = 4\nmax_pack_size = \"16GiB\"\n"
                       "\n[[rule]]\naction = \"compress\"\ninclude = [\"d/*\"]\ngroup = \"assets\"\n"
                       "\n[[rule]]\ninclude = \"x/*\"\nblock_size = 16384\ncompression_level = 5\n";
    auto config = lz4pack::config_from_toml(text);
    REQUIRE_OK(config);
    CHECK(config->default_action == lz4pack::Action::loose);
    CHECK_EQ(config->default_block_shift, 17u);
    CHECK_EQ(config->io_neutral_min_savings_bytes, 8192u);
    CHECK_EQ(config->auto_loose_min_file_size, 1572864u);
    CHECK_EQ(config->level, 12);
    CHECK_EQ(config->groups.size(), 2u); // assets, then default added
    CHECK(config->groups[0].name == "assets");
    CHECK_EQ(config->groups[0].max_pack_size, 16ull << 30);
    CHECK_EQ(config->rules.size(), 2u);
    CHECK_EQ(config->rules[0].block_shift, 17u); // inherits default_block_size
    CHECK(config->rules[0].group == "assets");
    CHECK(config->rules[1].action == lz4pack::Action::compress); // rule default
    CHECK_EQ(config->rules[1].include.size(), 1u);
    CHECK_EQ(config->rules[1].block_shift, 14u);
    CHECK_EQ(config->rules[1].level, 5);
    CHECK(!config->runtime);

    lz4pack::TomlOptions options;
    options.level = 9; // replaces [pack] compression_level, rules inherit it
    auto overridden = lz4pack::config_from_toml(text, options);
    REQUIRE_OK(overridden);
    CHECK_EQ(overridden->level, 9);
    CHECK_EQ(overridden->rules[0].level, 9);
    CHECK_EQ(overridden->rules[1].level, 5);

    CHECK(!lz4pack::config_from_toml("[[rule]]\ngroup = \"nope\"\n").ok());
    CHECK(!lz4pack::config_from_toml("[pack]\ndefault_block_size = 3000\n").ok());
    CHECK(!lz4pack::config_from_toml("[pack]\ndefault_action = \"zip\"\n").ok());
    CHECK(!lz4pack::config_from_toml("[runtime]\nworkers = 4\n").ok());
    CHECK(!lz4pack::config_from_toml("[[rule]]\ninclude_from = \"list.txt\"\n").ok()); // no file reader

    lz4pack::TomlOptions files;
    files.read_file = [](const std::string& name) -> Result<std::string> {
        if (name != "list.txt") return make_error(Errc::io, "missing");
        return std::string("# patterns\n  a/*  \n\nb/*\n");
    };
    auto from_file = lz4pack::config_from_toml("[[rule]]\ninclude_from = \"list.txt\"\n", files);
    REQUIRE_OK(from_file);
    CHECK_EQ(from_file->rules[0].include.size(), 2u); // no "**" when include_from is given
    CHECK(from_file->rules[0].include[0] == "a/*");
    CHECK(!lz4pack::config_from_toml("[[rule]]\ninclude_from = \"../x.txt\"\n", files).ok());
}

TEST_CASE(lz4_detects_engines_from_the_layout) {
    auto make = [](std::initializer_list<const char*> files, std::initializer_list<const char*> dirs) {
        tree::Node root;
        root.is_directory = true;
        auto add = [&](const std::string& path, bool dir) {
            tree::Node* node = &root;
            std::size_t start = 0;
            for (;;) {
                std::size_t slash = path.find('/', start);
                std::string part = path.substr(start, slash - start);
                tree::Node* child = nullptr;
                for (tree::Node& c : node->children)
                    if (c.name == part) child = &c;
                if (!child) {
                    node->children.push_back(tree::Node{});
                    child = &node->children.back();
                    child->name = part;
                    child->is_directory = slash != std::string::npos || dir;
                }
                node = child;
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
        };
        for (const char* d : dirs) add(d, true);
        for (const char* f : files) add(f, false);
        return root;
    };
    CHECK(lz4pack::detect_engine(make({"toc", "dag"}, {"d"})) == "insomniac");
    CHECK(lz4pack::detect_engine(make({"toc"}, {"d"})).empty());
    // PS5-LZ4-Compression's Unity layout and the PS5 Media/ layout.
    CHECK(lz4pack::detect_engine(make({"Data/globalgamemanagers"}, {"StreamingAssets"})) == "unity");
    CHECK(lz4pack::detect_engine(make({"Media/globalgamemanagers"}, {"Media/StreamingAssets"})) == "unity");
    CHECK(lz4pack::detect_engine(make({"Media/Metadata/global-metadata.dat"}, {"Media/StreamingAssets"})) == "unity");
    // Unity without StreamingAssets is still Unity: its data files must stay
    // loose, so the Unity profile (packing nothing) beats the scan profile.
    CHECK(lz4pack::detect_engine(make({"Media/globalgamemanagers"}, {"Media/Other"})) == "unity");
    CHECK(lz4pack::detect_engine(make({"Media/level0"}, {"Media/Other"})).empty());
}
