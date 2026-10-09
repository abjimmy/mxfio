/* SPDX-License-Identifier: GPL-2.0 */
#include "../pcie_owner.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>

struct Owner {
    int &destroyed;
    explicit Owner(int &count):destroyed(count) {}
    ~Owner() { ++destroyed; }
};
static void dangling_path(unsigned kind) {
    char directory[]="/tmp/gids-owner-links-XXXXXX";
    assert(mkdtemp(directory));
    std::string root=directory, parent=root+"/disk", device=parent+"/nvme0n1";
    assert(!mkdir(parent.c_str(),0700));
    int fd=open(device.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
    assert(fd>=0); assert(!close(fd));
    std::string alias=root+"/alias", middle=root+"/middle";
    // Direct alias, chained relative aliases, and a directory alias.
    assert(!symlink(kind==0 ? "disk/nvme0n1" : kind==1 ? "middle" : "disk",alias.c_str()));
    if(kind==1) assert(!symlink("disk/nvme0n1",middle.c_str()));
    std::string filename=kind==2 ? alias+"/nvme0n1" : alias;
    int created=0,destroyed=0;
    PcieOwnerCache<Owner> cache;
    auto create=[&](const std::string &resolved) {
        assert(resolved==device); ++created;
        assert(!unlink(device.c_str()));
        return std::make_shared<Owner>(destroyed);
    };
    auto owner=cache.acquire(filename.c_str(),create);
    auto late=cache.acquire(filename.c_str(),create);
    assert(owner==late && created==1);
    cache.release(late);
    std::string changed=kind==1 ? middle : alias;
    assert(!unlink(changed.c_str()));
    assert(!symlink("different-missing-device",changed.c_str()));
    bool rejected=false;
    try { cache.acquire(filename.c_str(),create); }
    catch(const std::runtime_error &) { rejected=true; }
    assert(rejected && created==1);
    cache.release(owner); assert(destroyed==1);
    assert(!unlink(alias.c_str()));
    if(kind==1) assert(!unlink(middle.c_str()));
    assert(!rmdir(parent.c_str())); assert(!rmdir(directory));
}
int main() {
    char directory[]="/tmp/gids-owner-XXXXXX";
    assert(mkdtemp(directory));
    std::string path=std::string(directory)+"/nvme0n1", alias=std::string(directory)+"/alias";
    auto touch=[](const std::string &name) {
        int fd=open(name.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
        assert(fd>=0); assert(!close(fd));
    };
    touch(path); assert(!symlink(path.c_str(),alias.c_str()));
    std::string replacement=std::string(directory)+"/replacement";
    touch(replacement);
    PcieOwnerCache<Owner> cache;
    int created=0,destroyed=0;
    auto create=[&](const std::string &resolved) {
        assert(resolved==path); ++created;
        // Driver takeover removes the original namespace node.
        assert(!unlink(path.c_str()));
        return std::make_shared<Owner>(destroyed);
    };
    auto first=cache.acquire(alias.c_str(),create);
    auto late=cache.acquire(path.c_str(),create);
    auto same_alias=cache.acquire(alias.c_str(),create);
    assert(first==late && first==same_alias && created==1);
    cache.release(first); cache.release(same_alias);
    assert(destroyed==0); // First job can exit while the late job keeps running.
    bool rejected=false;
    try { cache.acquire((std::string(directory)+"/other/nvme0n1").c_str(),create); }
    catch(const std::runtime_error &) { rejected=true; }
    assert(rejected); // A matching basename is not a known namespace path.
    assert(!unlink(alias.c_str()));
    assert(!symlink(replacement.c_str(),alias.c_str()));
    rejected=false;
    try { cache.acquire(alias.c_str(),create); }
    catch(const std::runtime_error &) { rejected=true; }
    assert(rejected); // A retargeted alias cannot silently select the old SSD.
    cache.release(late); assert(destroyed==1);
    touch(path);
    auto next=cache.acquire(path.c_str(),create);
    assert(created==2); // Restored device starts a new owner lifetime.
    cache.release(next); assert(destroyed==2);
    touch(path);
    rejected=false;
    try {
        cache.acquire(path.c_str(),[](const std::string &) -> std::shared_ptr<Owner> {
            throw std::runtime_error("initialization failed");
        });
    } catch(const std::runtime_error &) { rejected=true; }
    assert(rejected);
    next=cache.acquire(path.c_str(),create);
    assert(created==3); // Failed creation did not leave an owner or stale alias.
    cache.release(next); assert(destroyed==3);
    assert(!unlink(alias.c_str())); assert(!unlink(replacement.c_str()));
    assert(!rmdir(directory));
    for(unsigned kind=0;kind<3;++kind) dangling_path(kind);
    puts("PCIe owner paths, delayed reuse, replacement rejection and lifetime tests passed");
}
