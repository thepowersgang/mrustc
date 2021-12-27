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
    bool    dry_run;

    Opts()
        :patchfile(nullptr)
        ,base_dir(nullptr)
        ,dry_run(false)
    {
    }

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
        : orig_line(orig_line-1)
        , new_line(new_line-1)
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

    bool error = false;
    try {
        // 1. Parse the patch file
        auto patch = load_patch(opts.patchfile);
        // Sort fragments for easier processing
        for(auto& f : patch)
        {
            std::sort(f.fragments.begin(), f.fragments.end(), [&](const PatchFragment& a, const PatchFragment& b){ return a.orig_line < b.orig_line; });
        }

        // Iterate all files
        for(const auto& f : patch)
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
                std::cerr << "already patched: " << new_path << std::endl;
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
            {
                //size_t src_ofs = 0;
                for(const auto& frag : f.fragments)
                {
                    //for(size_t i = src_ofs; i < frag.orig_line; i ++)
                    //    std::cerr << i << "___ " << orig_file[i] << std::endl;
                    if( !sublist_match(orig_file, frag.orig_line, frag.orig_contents) )
                    {
                        if( frag.orig_line == frag.new_line && sublist_match(orig_file, frag.new_line, frag.new_contents) )
                        {
                            // Fragment is already applied
                        }
                        else
                        {
                            is_clean = false;
                        }
                    }

                    //for(size_t i = 0; i < frag.orig_contents.size(); i ++)
                    //    std::cerr << (frag.orig_line+i) << "--- " << frag.orig_contents[i] << std::endl;
                    //src_ofs = frag.orig_line + frag.orig_contents.size();
                }
                //for(size_t i = src_ofs; i < orig_file.size(); i ++)
                //    std::cerr << i << "___ " << orig_file[i] << std::endl;
            }

            if( !is_clean ) {
                std::cerr << "NOT CLEAN: " << orig_path << std::endl;
                error = true;
                continue;
            }
            std::cerr << "PATCHING: " << new_path << std::endl;

            // Apply each patch
            std::vector<std::string>    new_file2;
            size_t src_ofs = 0;
            for(const auto& frag : f.fragments)
            {
                new_file2.insert(new_file2.end(), orig_file.begin() + src_ofs, orig_file.begin() + frag.orig_line);
                new_file2.insert(new_file2.end(), frag.new_contents.begin(), frag.new_contents.end());
                src_ofs = frag.orig_line + frag.orig_contents.size();
            }
            new_file2.insert(new_file2.end(), orig_file.begin() + src_ofs, orig_file.end());

            // Save the new contents
            if( !opts.dry_run )
            {
                std::ofstream   ofs(new_path);
                for(const auto& line : new_file2)
                {
                    ofs << line << std::endl;
                }
                std::cerr << "`" << new_path << "` PATCHED" << std::endl;
            }
            else
            {
                std::cerr << "`" << new_path << "` to be PATCHED" << std::endl;
            }
        }
    }
    catch(const ::std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return error ? 1 : 0;
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
        if( !consume_while([](char c){ return std::isdigit(c) != 0; }) ) {
            throw ::std::runtime_error( std::string("Expected digit, found `") + *l + "`" );
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

            if(!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            if(!line.empty() && line.back() == '\r') {
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
                if(rv.empty())
                    throw ::std::runtime_error("`+++` without preceding ---");
                if(!(rv.back().new_path == ""))
                    throw ::std::runtime_error("`+++` without preceding ---");
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

                if(rv.empty())
                    throw ::std::runtime_error("@@ without preceding header");
                if(rv.back().new_path == "")
                    throw ::std::runtime_error("@@ without preceding header");

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

        return rv;
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
                std::cerr << "[" << (1+i+offset) << "] --- " << target[offset + i] << std::endl;
                std::cerr << "[" << (1+i+offset) << "] +++ " << pattern[i] << std::endl;
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

        if(arg[0] != '-' || rest_free) {
            if( !this->patchfile ) {
                this->patchfile = arg;
            }
            else if( !this->base_dir ) {
                this->base_dir = arg;
            }
            else {
                ::std::cerr << "Unexpected free argument `" << arg << "`" << ::std::endl;
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
                case 'n':
                    this->dry_run = true;
                    break;
                default:
                    ::std::cerr << "Unknown short argument -" << *arg << "" << ::std::endl;
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
            else if( std::strcmp(arg, "--dry-run") == 0 ) {
                this->dry_run = true;
            }
            else {
                ::std::cerr << "Unknown option " << arg << "" << ::std::endl;
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

    return true;
}
void Opts::usage(const char* name)
{
    ::std::cerr << "Usage: " << name << " file [base dir]" << ::std::endl;
}
void Opts::help(const char* name)
{
    this->usage(name);

    std::cerr << std::endl
        << "-n, --dry-run  : Don't make any changes" << std::endl
        ;
}
