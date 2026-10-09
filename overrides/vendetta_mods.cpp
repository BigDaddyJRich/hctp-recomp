// Def Jam Vendetta (USA, SLUS-20639) — the mods\ folder (see vendetta_mods.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_mods.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace vendetta_mods
{
    const Json *Json::find(const std::string &key) const
    {
        for (const auto &[name, value] : members)
            if (name == key)
                return &value;
        return nullptr;
    }

    namespace
    {
        class JsonReader
        {
        public:
            explicit JsonReader(const std::string &text) : m_text(text) {}

            bool read(Json &out, std::string &error)
            {
                if (!value(out))
                {
                    error = m_error;
                    return false;
                }
                space();
                if (m_pos != m_text.size())
                {
                    error = fail("text after the end of the JSON");
                    return false;
                }
                return true;
            }

        private:
            std::string fail(const std::string &what)
            {
                const size_t line = 1u + static_cast<size_t>(std::count(m_text.begin(), m_text.begin() + static_cast<std::ptrdiff_t>(std::min(m_pos, m_text.size())), '\n'));
                m_error = what + " (line " + std::to_string(line) + ")";
                return m_error;
            }

            void space()
            {
                while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
                    ++m_pos;
            }

            bool literal(const char *word)
            {
                const size_t length = std::char_traits<char>::length(word);
                if (m_text.compare(m_pos, length, word) != 0)
                    return false;
                m_pos += length;
                return true;
            }

            bool string(std::string &out)
            {
                ++m_pos; // the opening quote
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos++];
                    if (c == '"')
                        return true;
                    if (c != '\\')
                    {
                        out.push_back(c);
                        continue;
                    }
                    if (m_pos >= m_text.size())
                        break;
                    const char e = m_text[m_pos++];
                    switch (e)
                    {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        if (m_pos + 4u > m_text.size())
                        {
                            fail("a broken \\u escape");
                            return false;
                        }
                        const unsigned code = static_cast<unsigned>(std::strtoul(m_text.substr(m_pos, 4).c_str(), nullptr, 16));
                        m_pos += 4u;
                        // UTF-8 (surrogate pairs are not needed for names and paths)
                        if (code < 0x80u)
                            out.push_back(static_cast<char>(code));
                        else if (code < 0x800u)
                        {
                            out.push_back(static_cast<char>(0xC0u | (code >> 6u)));
                            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                        }
                        else
                        {
                            out.push_back(static_cast<char>(0xE0u | (code >> 12u)));
                            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
                            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                        }
                        break;
                    }
                    default:
                        fail(std::string("an unknown escape \\") + e);
                        return false;
                    }
                }
                fail("a string with no closing quote");
                return false;
            }

            bool value(Json &out)
            {
                space();
                if (m_pos >= m_text.size())
                {
                    fail("the text ends where a value should be");
                    return false;
                }
                const char c = m_text[m_pos];
                if (c == '{')
                {
                    out.type = Json::Type::Object;
                    ++m_pos;
                    space();
                    if (m_pos < m_text.size() && m_text[m_pos] == '}')
                    {
                        ++m_pos;
                        return true;
                    }
                    for (;;)
                    {
                        space();
                        if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                        {
                            fail("a name in quotes expected");
                            return false;
                        }
                        std::string key;
                        if (!string(key))
                            return false;
                        space();
                        if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                        {
                            fail("':' expected after \"" + key + "\"");
                            return false;
                        }
                        ++m_pos;
                        Json member;
                        if (!value(member))
                            return false;
                        out.members.emplace_back(std::move(key), std::move(member));
                        space();
                        if (m_pos < m_text.size() && m_text[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_text.size() && m_text[m_pos] == '}')
                        {
                            ++m_pos;
                            return true;
                        }
                        fail("',' or '}' expected");
                        return false;
                    }
                }
                if (c == '[')
                {
                    out.type = Json::Type::Array;
                    ++m_pos;
                    space();
                    if (m_pos < m_text.size() && m_text[m_pos] == ']')
                    {
                        ++m_pos;
                        return true;
                    }
                    for (;;)
                    {
                        Json item;
                        if (!value(item))
                            return false;
                        out.items.push_back(std::move(item));
                        space();
                        if (m_pos < m_text.size() && m_text[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_text.size() && m_text[m_pos] == ']')
                        {
                            ++m_pos;
                            return true;
                        }
                        fail("',' or ']' expected");
                        return false;
                    }
                }
                if (c == '"')
                {
                    out.type = Json::Type::String;
                    return string(out.text);
                }
                if (literal("true") || literal("false"))
                {
                    out.type = Json::Type::Bool;
                    out.boolean = m_text[m_pos - 1] == 'e' && m_text[m_pos - 4] == 't';
                    return true;
                }
                if (literal("null"))
                    return true;
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                {
                    const char *start = m_text.c_str() + m_pos;
                    char *end = nullptr;
                    out.type = Json::Type::Number;
                    out.number = std::strtod(start, &end);
                    m_pos += static_cast<size_t>(end - start);
                    return true;
                }
                fail(std::string("an unexpected '") + c + "'");
                return false;
            }

            const std::string &m_text;
            size_t m_pos = 0;
            std::string m_error;
        };

        bool modsOff()
        {
            const char *value = std::getenv("VENDETTA_MODS");
            return value && value[0] == '0';
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::vector<Mod> findMods()
        {
            std::vector<Mod> mods;
            if (modsOff())
            {
                std::cerr << "[mods] off (VENDETTA_MODS=0)" << std::endl;
                return mods;
            }
            const std::filesystem::path root = PS2Runtime::getIoPaths().elfDirectory.parent_path() / "mods";
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec))
                return mods;

            // every folder with a readable mod.json
            std::vector<Mod> found;
            for (const auto &entry : std::filesystem::directory_iterator(root, ec))
            {
                if (!entry.is_directory())
                    continue;
                Mod mod;
                mod.folder = entry.path();
                mod.folderName = entry.path().filename().string();
                std::ifstream file(mod.folder / "mod.json", std::ios::binary);
                if (!file)
                {
                    std::cerr << "[mods] " << mod.folderName << ": no mod.json - skipped" << std::endl;
                    continue;
                }
                std::stringstream text;
                text << file.rdbuf();
                std::string error;
                if (!parseJson(text.str(), mod.manifest, error) || !mod.manifest.isObject())
                {
                    std::cerr << "[mods] " << mod.folderName << ": mod.json can't be read: "
                              << (error.empty() ? "it isn't a JSON object" : error) << " - skipped" << std::endl;
                    continue;
                }
                const Json *name = mod.manifest.find("name");
                mod.name = name && name->isString() && !name->text.empty() ? name->text : mod.folderName;
                found.push_back(std::move(mod));
            }
            std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b)
                      { return lower(a.folderName) < lower(b.folderName); });

            // order.txt: the mods it names move to the end, in its order (later wins)
            std::vector<Mod> ordered;
            {
                std::ifstream order(root / "order.txt");
                std::vector<std::string> names;
                std::string line;
                while (std::getline(order, line))
                {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    if (!line.empty() && line[0] != '#')
                        names.push_back(lower(line));
                }
                std::vector<Mod> named;
                for (const std::string &want : names)
                {
                    auto it = std::find_if(found.begin(), found.end(), [&](const Mod &m)
                                           { return lower(m.folderName) == want || lower(m.name) == want; });
                    if (it == found.end())
                    {
                        std::cerr << "[mods] order.txt names \"" << want << "\", which isn't a mod in mods\\" << std::endl;
                        continue;
                    }
                    named.push_back(std::move(*it));
                    found.erase(it);
                }
                ordered = std::move(found);
                for (Mod &m : named)
                    ordered.push_back(std::move(m));
            }

            // "needs": a mod whose needs are missing is skipped; a needed mod loads first
            auto provides = [](const Mod &m, const std::string &want)
            { return lower(m.folderName) == lower(want) || lower(m.name) == lower(want); };
            std::vector<Mod> usable;
            for (Mod &mod : ordered)
            {
                bool ok = true;
                if (const Json *needs = mod.manifest.find("needs"); needs && needs->isArray())
                    for (const Json &need : needs->items)
                    {
                        if (!need.isString())
                            continue;
                        const bool present = std::any_of(ordered.begin(), ordered.end(), [&](const Mod &m)
                                                         { return &m != &mod && provides(m, need.text); });
                        if (!present)
                        {
                            std::cerr << "[mods] " << mod.name << " needs \"" << need.text
                                      << "\", which isn't in mods\\ - skipped" << std::endl;
                            ok = false;
                        }
                    }
                if (ok)
                    usable.push_back(std::move(mod));
            }
            // place each mod after everything it needs (stable otherwise; a cycle keeps the order it has)
            for (size_t pass = 0; pass < usable.size(); ++pass)
            {
                bool moved = false;
                for (size_t i = 0; i < usable.size() && !moved; ++i)
                {
                    const Json *needs = usable[i].manifest.find("needs");
                    if (!needs || !needs->isArray())
                        continue;
                    for (const Json &need : needs->items)
                    {
                        if (!need.isString())
                            continue;
                        for (size_t j = i + 1; j < usable.size(); ++j)
                            if (provides(usable[j], need.text))
                            {
                                Mod later = std::move(usable[j]);
                                usable.erase(usable.begin() + static_cast<std::ptrdiff_t>(j));
                                usable.insert(usable.begin() + static_cast<std::ptrdiff_t>(i), std::move(later));
                                moved = true;
                                break;
                            }
                        if (moved)
                            break;
                    }
                }
                if (!moved)
                    break;
            }
            for (const Mod &mod : usable)
                std::cerr << "[mods] loading " << mod.name << (mod.name != mod.folderName ? " (" + mod.folderName + ")" : "")
                          << std::endl;
            return usable;
        }
    }

    bool parseJson(const std::string &text, Json &out, std::string &error)
    {
        out = Json{};
        // a UTF-8 byte-order mark, as some editors write
        const std::string body = text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text;
        JsonReader reader(body);
        return reader.read(out, error);
    }

    const std::vector<Mod> &loadedMods()
    {
        static const std::vector<Mod> mods = findMods();
        return mods;
    }

    std::filesystem::path modFile(const Mod &mod, const std::string &relative, const char *what)
    {
        const std::filesystem::path path = (mod.folder / std::filesystem::path(relative)).lexically_normal();
        const std::string folder = mod.folder.lexically_normal().string();
        if (path.string().compare(0, folder.size(), folder) != 0)
        {
            std::cerr << "[mods] " << mod.name << ": " << what << " \"" << relative << "\" is outside the mod's folder - skipped"
                      << std::endl;
            return {};
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            std::cerr << "[mods] " << mod.name << ": " << what << " \"" << relative << "\" isn't in the mod's folder - skipped"
                      << std::endl;
            return {};
        }
        return path;
    }
}
