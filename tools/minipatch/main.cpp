//
//
//
#include <vector>
#include <string>
#include <fstream>
#include <cctype>
#include <iostream>
#include <algorithm>

struct Opts
{
    const char* patchfile;
    const char* base_dir;

    bool parse(int argc, char *argv[]);
    void usage(const char* name);
    void help(const char* name);
};

struct PatchFragment
{
    unsigned    orig_line;
    unsigned    new_line;

    std::vector<std::string>    orig_contents;
    std::vector<std::string>    new_contents;

    PatchFragment(unsigned orig_line, unsigned orig_count, unsigned new_line, unsigned new_count)
        : orig_line(orig_line)
        , new_line(new_line)
    {
        orig_contents.reserve(orig_count);
        new_contents.reserve(new_count);
    }
};
struct FilePatch
{
    std::string orig_path;
    std::string new_path;
    std::vector<PatchFragment>  fragments;

    FilePatch(std::string path)
        : orig_path(path)
    {
    }
};

namespace {
    std::vector<FilePatch> load_patch(const char* patchfile_path);
    std::vector<std::string>    load_file(const char* path);

    bool sublist_match(const std::vector<::std::string>& target, size_t offset, const std::vector<::std::string>& pattern);
}

int main(int argc, char *argv[])
{
    Opts    opts;

    if( !opts.parse(argc, argv) )
    {
        return 1;
    }

    try {
        // 1. Parse the patch file
        auto patch = load_patch(opts.patchfile);

        // Iterate all files
        for(auto& f : patch)
        {
            // Get the output path
            std::string new_path;
            new_path.append(opts.base_dir);
            if(opts.base_dir)
                new_path.append("/");
            new_path.append(f.new_path.c_str());

            // Check if the patches are applied, and apply each
            bool is_applied = true;
            auto new_file = load_file(new_path.c_str());
            for(const auto& frag : f.fragments)
            {
                is_applied &= sublist_match(new_file, frag.new_line, frag.new_contents);
            }

            if( is_applied ) {
                std::cerr << new_path << " aready patched" << std::endl;
                continue;
            }


            // Get the input path
            std::string orig_path;
            orig_path.append(opts.base_dir);
            if(opts.base_dir)
                orig_path.append("/");
            orig_path.append(f.orig_path.c_str());

            auto orig_file = load_file(orig_path.c_str());
            bool is_clean = true;
            for(const auto& frag : f.fragments)
            {
                is_clean &= sublist_match(new_file, frag.orig_line, frag.orig_contents);
            }

            if( !is_clean ) {
                std::cerr << orig_path << " is not clean" << std::endl;
                continue;
            }

            // Apply each patch
            std::sort(f.fragments.begin(), f.fragments.end(), [&](const PatchFragment& a, const PatchFragment& b){ return a.orig_line < b.orig_line; });
            std::vector<std::string>    new_file2;
            size_t src_ofs = 0;
            for(const auto& frag : f.fragments)
            {
                new_file2.insert(new_file2.end(), orig_file.begin() + src_ofs, orig_file.begin() + frag.orig_line);
                new_file2.insert(new_file2.begin(), frag.new_contents.begin(), frag.new_contents.end());
                src_ofs = frag.orig_line + frag.orig_contents.size();
            }

            // Save the new contents
            {
                std::ofstream   ofs(new_path);
                for(const auto& line : new_file2)
                {
                    ofs << line << std::endl;
                }
            }
            std::cerr << new_path << " patched" << std::endl;
        }
    }
    catch(const ::std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}

struct Parser {
    const char* l;

    Parser(const std::string& s)
        : l(s.c_str())
    {
    }

    bool is_eof() const {
        return *l == '\0';
    }
    void expect_eof() const {
        if( !is_eof() ) {
            throw ::std::runtime_error("TODO: error message");
        }
    }

    void consume_whitespace() {
        while( isblank(*l) )    l ++;
    }
    bool try_consume(char v) {
        if(*l == v) {
            l ++;
            return true;
        }
        else {
            return false;
        }
    }
    bool try_consume(const char* v) {
        auto len = strlen(v);
        if( strncmp(l, v, len) == 0 ) {
            l += len;
            return true;
        }
        else {
            return false;
        }
    }
    void expect_consume(const char* v) {
        auto len = strlen(v);
        if( strncmp(l, v, len) == 0 ) {
            l += len;
        }
        else {
            throw ::std::runtime_error("TODO: error message");
        }
    }

    bool consume_while(bool (*cb)(char)) {
        bool rv = cb(*l);
        do {
            l ++;
        } while(cb(*l));
        return rv;
    }

    unsigned read_int() {
        const char* s = l;
        if( !consume_while([](char c){ return std::isdigit(c) == 0; }) ) {
            throw ::std::runtime_error("Expected digit, found TODO");
        }
        const char* e = l;
        return std::stol( std::string(s, e) );
    }
};

namespace {
    std::vector<FilePatch> load_patch(const char* patchfile_path)
    {
        std::ifstream   ifs(patchfile_path);
        if(!ifs.good()) {
            throw ::std::runtime_error("Unable to open patch file");
        }

        std::vector<FilePatch>  rv;
        std::string line;
        while(!ifs.eof())
        {
            std::getline(ifs, line);

            while( !line.empty() && isblank(line.back()) ) {
                line.pop_back();
            }

            if( line.empty() ) {
                continue;
            }

            Parser  p(line);
            if( p.try_consume("---") ) {
                p.consume_whitespace();
                rv.push_back(FilePatch(p.l));
            }
            else if( p.try_consume("+++") ) {
                p.consume_whitespace();
                if(!rv.empty())
                    throw ::std::runtime_error("");
                if(!(rv.back().new_path == ""))
                    throw ::std::runtime_error("");
                rv.back().new_path = p.l;
            }
            else if( p.try_consume("@@") ) {
                p.consume_whitespace();
                p.expect_consume("-");
                auto orig_line = p.read_int();
                p.expect_consume(",");
                auto orig_len = p.read_int();
                p.consume_whitespace();
                p.expect_consume("+");
                auto new_line = p.read_int();
                p.expect_consume(",");
                auto new_len = p.read_int();
                p.consume_whitespace();
                p.expect_consume("@@");
                p.expect_eof();

                if(!rv.empty())
                    throw ::std::runtime_error("");
                if(rv.back().new_path == "")
                    throw ::std::runtime_error("");

                rv.back().fragments.push_back(PatchFragment(orig_line, orig_len, new_line, new_len));
            }
            else {
                switch(line[0])
                {
                case '+':
                    rv.back().fragments.back().new_contents.push_back(line.c_str()+1);
                    break;
                case '-':
                    rv.back().fragments.back().orig_contents.push_back(line.c_str()+1);
                    break;
                case ' ':
                    // Common line
                    rv.back().fragments.back().new_contents.push_back(line.c_str()+1);
                    rv.back().fragments.back().orig_contents.push_back(line.c_str()+1);
                    break;
                default:
                    // ignore the line
                    break;
                }
            }

        }
    }
    std::vector<std::string> load_file(const char* path)
    {
        std::ifstream   ifs(path);
        if(!ifs.good()) {
            throw ::std::runtime_error("Unable to open patch file");
        }

        std::vector<std::string>  rv;
        std::string line;
        while(!ifs.eof())
        {
            std::getline(ifs, line);

            if(!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            if(!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            rv.push_back(std::move(line));
        }
        return rv;
    }

    bool sublist_match(const std::vector<::std::string>& target, size_t offset, const std::vector<::std::string>& pattern)
    {
        if( offset >= target.size() ) {
            return false;
        }
        if( offset + pattern.size() >= target.size() ) {
            return false;
        }

        for(size_t i = 0; i < pattern.size(); i ++)
        {
            if( target[offset + i] != pattern[i] )
            {
                return false;
            }
        }
        return true;
    }
}

bool Opts::parse(int argc, char *argv[])
{
    bool rest_free = false;
    for(int argi = 1; argi < argc; argi++)
    {
        const char* arg = argv[argi];

        if(arg[0] != '-') {
            if( !this->patchfile ) {
                this->patchfile = arg;
            }
            else if( !this->base_dir ) {
                this->base_dir = arg;
            }
            else {
                this->usage(argv[0]);
                return false;
            }
        }
        else if( arg[1] != '-' ) {
            while(*++arg) {
                switch(*arg)
                {
                case 'h':
                    this->help(argv[0]);
                    std::exit(0);
                default:
                    this->usage(argv[0]);
                    return false;
                }
            }
        }
        else if( arg[2] != '\0' ) {
            if( std::strcmp(arg, "--help") == 0 ) {
                this->help(argv[0]);
                std::exit(0);
            }
            else {
                this->usage(argv[0]);
                return false;
            }
        }
        else {
            rest_free = true;
        }
    }

    if( !this->patchfile ) {
        this->usage(argv[0]);
        return false;
    }
    if( !this->base_dir ) {
        this->usage(argv[0]);
        return false;
    }

    return true;
}
void Opts::usage(const char* name)
{

}
void Opts::help(const char* name)
{
    this->usage(name);
}
