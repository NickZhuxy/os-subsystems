#include "format.hpp"
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace os::fs {
CheckReport check(const std::string& path) {
    CheckReport report;
    auto issue = [&](const std::string& text) {
        if (report.issues.size() < 1000) report.issues.push_back(text);
    };
    try {
        disk::Reader reader(path,false); const auto& s = reader.super;
        // This checker never uses Image's allocation, path, or block-mapping code.
        std::unordered_map<std::uint32_t,disk::Block> metadata;
        for (std::uint32_t id = 1; id < s.data_start; ++id) metadata.emplace(id,reader.raw_block(id));
        auto bit = [&](std::uint32_t start, std::uint32_t id) {
            return (metadata.at(start + id / 32768)[(id % 32768) / 8] & (1U << (id % 8))) != 0;
        };
        if (!bit(s.inode_bitmap,0)) issue("reserved inode zero is free");
        if (!bit(s.inode_bitmap,1)) issue("root inode is free");
        std::vector<disk::Inode> nodes(s.inodes);
        std::vector<bool> allocated(s.inodes), reachable(s.inodes);
        std::vector<std::uint64_t> links(s.inodes);
        std::vector<std::uint32_t> owners(s.blocks), parents(s.inodes);
        std::map<std::uint32_t,std::vector<DirectoryEntry>> directories;
        for (std::uint32_t id = 0; id < s.data_start; ++id)
            if (!bit(s.block_bitmap,id)) issue("metadata block marked free: " + std::to_string(id));
        auto own = [&](std::uint32_t block, std::uint32_t inode) {
            if (block < s.data_start || block >= s.blocks) {
                issue("invalid block pointer in inode " + std::to_string(inode)); return false;
            }
            if (owners[block]) {
                issue("multiply referenced block " + std::to_string(block)); return false;
            }
            owners[block] = inode; ++report.referenced_blocks;
            if (!bit(s.block_bitmap,block)) issue("referenced block marked free: " + std::to_string(block));
            return true;
        };
        for (std::uint32_t id = 1; id < s.inodes; ++id) {
            auto position = static_cast<std::uint64_t>(id) * disk::inode_size;
            nodes[id] = disk::decode_inode(std::span(metadata.at(s.inode_table + position / block_size))
                .subspan(position % block_size,disk::inode_size));
            allocated[id] = bit(s.inode_bitmap,id);
            auto& node = nodes[id];
            if (!allocated[id]) {
                if (node.type || node.size || node.links || node.single || node.indirect ||
                    std::any_of(node.direct.begin(),node.direct.end(),[](auto value) { return value != 0; }))
                    issue("free inode contains live metadata: " + std::to_string(id));
                continue;
            }
            ++report.allocated_inodes;
            if (node.type != 1 && node.type != 2) { issue("invalid inode type: " + std::to_string(id)); continue; }
            if (!node.generation) issue("zero inode generation: " + std::to_string(id));
            if (node.size > max_file_size) issue("inode size exceeds limit: " + std::to_string(id));
            std::map<std::uint64_t,std::uint32_t> data;
            auto leaf = [&](std::uint32_t block, std::uint64_t logical) {
                if (!block) return;
                if (logical >= (node.size + block_size - 1) / block_size)
                    issue("data pointer beyond EOF in inode " + std::to_string(id));
                if (own(block,id)) data.emplace(logical,block);
            };
            auto single = [&](std::uint32_t block, std::uint64_t first) {
                if (!block || !own(block,id)) return;
                auto table = reader.raw_block(block); bool any = false;
                for (std::size_t i = 0; i < 1024; ++i) {
                    auto child = static_cast<std::uint32_t>(disk::get(table,i * 4,4));
                    any = any || child != 0; leaf(child,first + i);
                }
                if (!any) issue("empty indirect block in inode " + std::to_string(id));
            };
            for (std::size_t i = 0; i < 8; ++i) leaf(node.direct[i],i);
            single(node.single,8);
            if (node.indirect && own(node.indirect,id)) {
                auto table = reader.raw_block(node.indirect); bool any = false;
                for (std::size_t i = 0; i < 1024; ++i) {
                    auto child = static_cast<std::uint32_t>(disk::get(table,i * 4,4));
                    any = any || child != 0; single(child,8 + 1024 + i * 1024);
                }
                if (!any) issue("empty double-indirect block in inode " + std::to_string(id));
            }
            if (node.type != 2) continue;
            if (node.size < 128 || node.size % disk::directory_record_size || node.size > disk::max_directory_size) {
                issue("invalid directory size: " + std::to_string(id)); continue;
            }
            std::set<std::string> names; bool dot = false, up = false;
            auto& entries = directories[id];
            for (std::uint64_t logical = 0; logical * block_size < node.size; ++logical) {
                auto found = data.find(logical);
                if (found == data.end()) { issue("hole in directory " + std::to_string(id)); continue; }
                auto block = reader.raw_block(found->second);
                for (std::size_t offset = 0; offset < block_size && logical * block_size + offset < node.size;
                     offset += disk::directory_record_size) {
                    try {
                        auto entry = disk::decode_record(std::span(block).subspan(offset,disk::directory_record_size));
                        if (!entry.inode) continue;
                        if (!names.insert(entry.name).second) issue("duplicate directory name in inode " + std::to_string(id));
                        if (entry.inode >= s.inodes) { issue("directory target outside image"); continue; }
                        ++links[entry.inode];
                        if (entry.name == ".") { dot = true; if (entry.inode != id) issue("incorrect dot entry"); }
                        if (entry.name == "..") up = true;
                        entries.push_back(std::move(entry));
                    } catch (const std::system_error& error) { issue(error.what()); }
                }
            }
            if (!dot || !up) issue("missing dot entries in inode " + std::to_string(id));
        }
        if (!allocated[1] || nodes[1].type != 2) issue("root is not an allocated directory");
        parents[1] = 1;
        for (const auto& [id,entries] : directories) for (const auto& entry : entries) {
            if (!allocated[entry.inode]) issue("directory points to a free inode: " + std::to_string(entry.inode));
            if (entry.name == "." || entry.name == "..") continue;
            if (nodes[entry.inode].type == 2) {
                if (parents[entry.inode]) issue("directory has multiple parents: " + std::to_string(entry.inode));
                else parents[entry.inode] = id;
            }
        }
        for (const auto& [id,entries] : directories) for (const auto& entry : entries)
            if (entry.name == ".." && entry.inode != parents[id]) issue("incorrect parent entry in inode " + std::to_string(id));
        std::vector<std::uint32_t> stack{1};
        while (!stack.empty()) {
            auto id = stack.back(); stack.pop_back();
            if (reachable[id] || !allocated[id]) continue;
            reachable[id] = true; ++report.reachable_inodes;
            auto found = directories.find(id);
            if (found != directories.end()) for (const auto& entry : found->second)
                if (entry.name != "." && entry.name != "..") stack.push_back(entry.inode);
        }
        for (std::uint32_t id = 1; id < s.inodes; ++id) if (allocated[id]) {
            if (!reachable[id]) issue("unreachable allocated inode: " + std::to_string(id));
            if (nodes[id].links != links[id]) issue("link count mismatch in inode " + std::to_string(id));
        }
        for (std::uint32_t id = s.data_start; id < s.blocks; ++id)
            if (bit(s.block_bitmap,id) && !owners[id]) issue("allocated block is unreferenced: " + std::to_string(id));
    } catch (const std::exception& error) { issue(error.what()); }
    return report;
}
}
