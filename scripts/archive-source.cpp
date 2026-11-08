// SPDX-License-Identifier: GPL-3.0-or-later
// Assemble a deterministic source ZIP exclusively from tracked Git revisions,
// recursively including every pinned submodule. No working-tree files are read.
#include <archive.h>
#include <archive_entry.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
static void require(bool b,const std::string& why){if(!b)throw std::runtime_error(why);}
static std::string strip(std::string s){
    const auto p=s.find_first_not_of(" \t\r\n");
    if(p==std::string::npos)return {};
    return s.substr(p,s.find_last_not_of(" \t\r\n")-p+1);
}
// Shell-free, cwd-independent binary Git invocation.
static std::string git(const fs::path& repo,const std::vector<std::string>& options){
    int pipes[2];
    if(pipe(pipes)!=0)throw std::runtime_error("pipe: "+std::string(strerror(errno)));
    const pid_t child=fork();
    if(child<0){close(pipes[0]);close(pipes[1]);throw std::runtime_error("fork failed");}
    if(child==0){
        close(pipes[0]);
        if(dup2(pipes[1],STDOUT_FILENO)<0)_exit(127);
        close(pipes[1]);
        std::vector<std::string> owned{"git","-C",repo.string()};
        owned.insert(owned.end(),options.begin(),options.end());
        std::vector<char*> argv;
        for(auto& arg:owned)argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp("git",argv.data());
        _exit(127);
    }
    close(pipes[1]);
    std::string output;
    std::array<char,32768> buffer{};
    bool oversize=false,ioerror=false;
    while(true){
        const ssize_t n=read(pipes[0],buffer.data(),buffer.size());
        if(n==0)break;
        if(n<0){if(errno==EINTR)continue;ioerror=true;break;}
        if(output.size()+std::size_t(n)>256*1024*1024){oversize=true;break;}
        output.append(buffer.data(),std::size_t(n));
    }
    close(pipes[0]);
    int status=0;
    while(waitpid(child,&status,0)<0){if(errno!=EINTR)throw std::runtime_error("waitpid failed");}
    require(!oversize&&!ioerror&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
            "Git command failed or exceeded source safety limit in "+repo.string());
    return output;
}
using Reader=std::unique_ptr<archive,decltype(&archive_read_free)>;
using Writer=std::unique_ptr<archive,decltype(&archive_write_free)>;
static std::string archive_error(archive* a){
    return archive_error_string(a)?archive_error_string(a):"unknown archive error";
}
static std::string read_member(archive* a){
    std::string out;
    std::array<char,32768> buffer{};
    while(true){
        const la_ssize_t n=archive_read_data(a,buffer.data(),buffer.size());
        if(n==0)break;
        require(n>0,archive_error(a));
        require(out.size()+std::size_t(n)<256*1024*1024,"archive member exceeds safety limit");
        out.append(buffer.data(),std::size_t(n));
    }
    return out;
}
static void append(archive* writer,const fs::path& repo,const std::string& revision,
                   const std::string& prefix,std::set<std::string>& entries,unsigned depth){
    require(depth<=12,"submodule nesting exceeds limit");
    const std::string tar=git(repo,{"archive","--format=tar","--prefix="+prefix,revision});
    Reader reader(archive_read_new(),&archive_read_free);
    require(bool(reader),"could not create archive reader");
    require(archive_read_support_format_tar(reader.get())==ARCHIVE_OK,"tar decoding unsupported");
    require(archive_read_open_memory(reader.get(),tar.data(),tar.size())==ARCHIVE_OK,
            "cannot decode tracked Git archive: "+archive_error(reader.get()));
    archive_entry* entry=nullptr;
    while(true){
        const int result=archive_read_next_header(reader.get(),&entry);
        if(result==ARCHIVE_EOF)break;
        require(result==ARCHIVE_OK,"Git archive header: "+archive_error(reader.get()));
        const char* pathname=archive_entry_pathname(entry);
        require(pathname&&*pathname,"Git archive has unnamed entry");
        const std::string name(pathname);
        const bool directory=archive_entry_filetype(entry)==AE_IFDIR;
        if(!entries.insert(name).second){
            require(directory,"Duplicate source ZIP member: "+name);
            continue;
        }
        archive_entry* copy=archive_entry_clone(entry);
        require(copy,"cannot copy Git archive entry");
        archive_entry_set_uid(copy,0);
        archive_entry_set_gid(copy,0);
        archive_entry_set_uname(copy,"");
        archive_entry_set_gname(copy,"");
        archive_entry_unset_atime(copy);
        archive_entry_unset_ctime(copy);
        const int header=archive_write_header(writer,copy);
        archive_entry_free(copy);
        require(header==ARCHIVE_OK,"ZIP entry "+name+": "+archive_error(writer));
        if(!directory){
            std::array<char,32768> buffer{};
            while(true){
                const la_ssize_t n=archive_read_data(reader.get(),buffer.data(),buffer.size());
                if(n==0)break;
                require(n>0,"cannot read tracked file "+name);
                const la_ssize_t written=archive_write_data(writer,buffer.data(),std::size_t(n));
                require(written==n,"cannot write ZIP file "+name);
            }
        }
        require(archive_write_finish_entry(writer)==ARCHIVE_OK,
                "cannot finish ZIP entry "+name);
    }
    const std::string tree=git(repo,{"ls-tree","-rz",revision});
    std::size_t start=0;
    while(start<tree.size()){
        const std::size_t end=tree.find('\0',start);
        require(end!=std::string::npos,"invalid Git tree record");
        const std::string record=tree.substr(start,end-start);
        const auto tab=record.find('\t');
        require(tab!=std::string::npos,"invalid Git tree metadata");
        const std::string meta=record.substr(0,tab),relative=record.substr(tab+1);
        if(meta.rfind("160000 commit ",0)==0){
            const std::string sha=meta.substr(14);
            require(std::regex_match(sha,std::regex("[0-9a-f]{40}")),
                    "invalid submodule identity");
            const fs::path child=repo/relative;
            require(fs::is_directory(child),"submodule not initialized: "+relative);
            require(strip(git(child,{"rev-parse","HEAD"}))==sha,
                    "submodule checkout does not match gitlink: "+relative);
            append(writer,child,sha,prefix+relative+"/",entries,depth+1);
        }
        start=end+1;
    }
}
static void build(const fs::path& repo,const fs::path& output){
    const std::string commit=strip(git(repo,{"rev-parse","HEAD"}));
    const std::string version=strip(git(repo,{"show",commit+":VERSION"}));
    require(std::regex_match(version,std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")),
            "Invalid source VERSION");
    if(!output.parent_path().empty())fs::create_directories(output.parent_path());
    const fs::path temporary=output.string()+".XXXXXX";
    std::string tmp=temporary.string();
    std::vector<char> pattern(tmp.begin(),tmp.end());pattern.push_back(0);
    const int fd=mkstemp(pattern.data());
    require(fd>=0,"cannot create temporary ZIP");
    close(fd);
    const fs::path staged=pattern.data();
    try {
        Writer writer(archive_write_new(),&archive_write_free);
        require(bool(writer),"cannot create ZIP writer");
        require(archive_write_set_format_zip(writer.get())==ARCHIVE_OK,"ZIP format unavailable");
        require(archive_write_zip_set_compression_deflate(writer.get())==ARCHIVE_OK,
                "ZIP deflate unavailable");
        require(archive_write_open_filename(writer.get(),staged.c_str())==ARCHIVE_OK,
                archive_error(writer.get()));
        std::set<std::string> entries;
        append(writer.get(),repo,commit,"Link-"+version+"/",entries,0);
        require(archive_write_close(writer.get())==ARCHIVE_OK,
                "cannot finish ZIP: "+archive_error(writer.get()));
        fs::rename(staged,output);
    }catch(...){std::error_code ec;fs::remove(staged,ec);throw;}
}
static void verify(const fs::path& repo,const fs::path& zip,const std::string& version){
    require(std::regex_match(version,std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")),
            "Invalid source VERSION");
    Reader reader(archive_read_new(),&archive_read_free);
    require(bool(reader),"could not create ZIP reader");
    require(archive_read_support_format_zip(reader.get())==ARCHIVE_OK,"ZIP unsupported");
    require(archive_read_open_filename(reader.get(),zip.c_str(),16384)==ARCHIVE_OK,
            "cannot inspect source ZIP: "+archive_error(reader.get()));
    const std::string required="Link-"+version+"/src/infiltratr-common/VERSION";
    std::set<std::string> entries;
    bool found=false;
    archive_entry* entry=nullptr;
    while(true){
        const int r=archive_read_next_header(reader.get(),&entry);
        if(r==ARCHIVE_EOF)break;
        require(r==ARCHIVE_OK,"ZIP entry: "+archive_error(reader.get()));
        const std::string name=archive_entry_pathname(entry);
        require(name.find(".archive-should-not-ship")==std::string::npos,
                "untracked file leaked into ZIP: "+name);
        require(entries.insert(name).second,"duplicate ZIP member: "+name);
        if(name==required){
            std::ifstream actual(repo/"src/infiltratr-common/VERSION",std::ios::binary);
            require(bool(actual),"submodule VERSION unavailable");
            const std::string expected{std::istreambuf_iterator<char>(actual),{}};
            require(read_member(reader.get())==expected,"pinned submodule VERSION changed");
            found=true;
        }else require(archive_read_data_skip(reader.get())==ARCHIVE_OK,
                     "cannot skip ZIP entry: "+name);
    }
    require(found,"pinned Common VERSION missing from source ZIP");
}
int main(int argc,char**argv){
    try {
        const fs::path repo=fs::current_path();
        if(argc==2){build(repo,argv[1]);std::cout<<argv[1]<<'\n';return 0;}
        if(argc==4&&std::string(argv[1])=="--verify"){
            verify(repo,argv[2],argv[3]);std::cout<<"Source ZIP integrity: PASS\n";return 0;
        }
        std::cerr<<"Usage: archive-source OUTPUT.zip | --verify OUTPUT.zip VERSION\n";
        return 2;
    }catch(const std::exception&error){
        std::cerr<<"archive-source: "<<error.what()<<'\n';
        return 1;
    }
}
