/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GIDS_PCIE_OWNER_HPP
#define GIDS_PCIE_OWNER_HPP
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Remember only paths resolved before controller takeover. The nvme block
// node disappears on rebind; an active owner remains usable through its saved
// paths. Weak references neither extend ownership nor survive final release.
template<class Owner> class PcieOwnerCache {
    struct PathNode {
        std::string path, link;
        struct stat node;
    };
    struct Entry {
        std::weak_ptr<Owner> owner;
        std::vector<PathNode> chain;
    };
    std::mutex lock;
    std::map<std::string,Entry> paths;

    static std::string link_target(const std::string &path) {
        char target[PATH_MAX];
        auto size=readlink(path.c_str(),target,sizeof(target));
        if(size<0 || size==PATH_MAX)
            throw std::runtime_error("read PCIe namespace link failed: "+path);
        return std::string(target,size);
    }
    static void remember(const std::string &path,std::vector<PathNode> &chain,
                         std::set<std::string> &seen) {
        // Record each ancestor and follow link targets too, including relative
        // targets containing other links. realpath() has already rejected loops.
        for(size_t end=1;end<=path.size();++end) {
            if(end!=path.size() && path[end]!='/') continue;
            std::string part=path.substr(0,end);
            if(!seen.insert(part).second) continue;
            PathNode item{part,{}, {}};
            if(lstat(part.c_str(),&item.node))
                throw std::runtime_error("inspect PCIe namespace path failed: "+part);
            if(S_ISLNK(item.node.st_mode)) item.link=link_target(part);
            chain.push_back(item);
            if(!item.link.empty()) {
                if(chain.size()>256) throw std::runtime_error("PCIe namespace path is too complex");
                remember(item.link[0]=='/' ? item.link :
                         part.substr(0,part.find_last_of('/')+1)+item.link,chain,seen);
            }
        }
    }
    static std::vector<PathNode> remember(const std::string &path) {
        std::vector<PathNode> chain;
        std::set<std::string> seen;
        remember(path,chain,seen);
        return chain;
    }
    std::shared_ptr<Owner> find(const std::string &path) {
        auto it=paths.find(path);
        if(it==paths.end()) return {};
        auto owner=it->second.owner.lock();
        if(!owner) { paths.erase(it); return {}; }
        for(const auto &saved:it->second.chain) {
            struct stat node{};
            if(lstat(saved.path.c_str(),&node)) {
                // Rebind can remove a block node and its udev aliases, but
                // must not remove their parent directories. Existing links
                // are checked even when their new targets do not exist.
                if(errno==ENOENT && !S_ISDIR(saved.node.st_mode)) continue;
                throw std::runtime_error("inspect owned PCIe namespace path failed: "+saved.path);
            }
            const auto &old=saved.node;
            if(node.st_dev!=old.st_dev || node.st_ino!=old.st_ino ||
               node.st_rdev!=old.st_rdev || (node.st_mode&S_IFMT)!=(old.st_mode&S_IFMT) ||
               (S_ISLNK(node.st_mode) && link_target(saved.path)!=saved.link))
                throw std::runtime_error("PCIe namespace path changed while its controller is owned: "+path);
        }
        return owner;
    }
public:
    template<class Create> std::shared_ptr<Owner> acquire(const char *filename,Create create) {
        if(!filename || !*filename) throw std::runtime_error("empty PCIe namespace path");
        std::string path=filename;
        if(path[0]!='/') {
            char cwd[PATH_MAX];
            if(!getcwd(cwd,sizeof(cwd))) throw std::runtime_error("get namespace working directory failed");
            path=std::string(cwd)+"/"+path;
        }
        // Serialize lookup, first takeover and final driver restoration.
        std::lock_guard<std::mutex> guard(lock);
        if(auto owner=find(path)) return owner;
        char resolved[PATH_MAX];
        if(!realpath(path.c_str(),resolved))
            throw std::runtime_error("resolve NVMe namespace: "+std::string(strerror(errno)));
        auto chain=remember(path), canonical=remember(resolved);
        auto owner=find(resolved);
        if(!owner) owner=create(std::string(resolved));
        paths[path]={owner,std::move(chain)};
        paths[resolved]={owner,std::move(canonical)};
        return owner;
    }
    void release(std::shared_ptr<Owner> &owner) {
        std::lock_guard<std::mutex> guard(lock);
        owner.reset();
        for(auto it=paths.begin();it!=paths.end();) {
            if(it->second.owner.expired()) it=paths.erase(it);
            else ++it;
        }
    }
};
#endif
