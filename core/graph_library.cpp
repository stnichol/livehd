//  This file is distributed under the BSD 3-Clause License. See LICENSE for details.

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"

#include <cassert>
#include <fstream>
#include <regex>
#include <set>

#include "fmt/format.h"

#include "graph_library.hpp"
#include "lgraph.hpp"
#include "pass.hpp"

Graph_library::Global_instances   Graph_library::global_instances;
Graph_library::Global_name2lgraph Graph_library::global_name2lgraph;

Graph_library *Graph_library::instance(std::string_view path) {
#if 0
  auto it = Graph_library::global_instances.find(path);
  if(it != Graph_library::global_instances.end()) {
    return it->second;
  }
#endif

  std::string spath(path);

  char  full_path[PATH_MAX + 1];
  char *ptr = realpath(spath.c_str(), full_path);
  if (ptr == nullptr) {
    mkdir(spath.c_str(), 0755);  // At least make sure directory exists for future
    ptr = realpath(spath.c_str(), full_path);
    I(ptr);
  }

  auto it = Graph_library::global_instances.find(full_path);
  if (it != Graph_library::global_instances.end()) {
    return it->second;
  }

  Graph_library *graph_library = new Graph_library(full_path);
  Graph_library::global_instances.insert(std::make_pair(std::string(full_path), graph_library));

  return graph_library;
}

Lg_type_id Graph_library::reset_id(std::string_view name, std::string_view source) {
  graph_library_clean = false;

  const auto &it = name2id.find(name);
  if (it != name2id.end()) {
    // Maybe it was a sub before, or reloaded, or the ID got recycled
    attribute[it->second].version = max_next_version.value++;
    if (attribute[it->second].source != source) {
      if (attribute[it->second].source == "-") {
        attribute[it->second].source = source;
        Pass::warn("overwrite lgraph:{} source from {} to {}", name, attribute[it->second].source, source);  // LCOV_EXCL_LINE
      } else if (source == "-") {
        Pass::warn("keeping lgraph:{} source {}", name, attribute[it->second].source);  // LCOV_EXCL_LINE
      } else if (attribute[it->second].source.empty()) {
        // Blackbox with a newly populated. OK
        attribute[it->second].source = source;
      } else {
        Pass::error("No overwrite lgraph:{} because it changed source from {} to {} (LGraph::delete first)", name, attribute[it->second].source, source);  // LCOV_EXCL_LINE
      }
    }
    return it->second;
  }
  return add_name(name, source);
}

bool Graph_library::exists(std::string_view path, std::string_view name) {
  const Graph_library *lib = instance(path);

  return lib->name2id.find(name) != lib->name2id.end();
}

LGraph *Graph_library::try_find_lgraph(std::string_view path, std::string_view name) {
  Graph_library *lib = instance(path);
  return lib->try_find_lgraph(name);
}

LGraph *Graph_library::try_find_lgraph(std::string_view name) {
  if (global_name2lgraph.find(path) == global_name2lgraph.end()) {
    return nullptr;  // Library exists, but not the instance for lgraph
  }

  if (global_name2lgraph[path].find(name) != global_name2lgraph[path].end()) {
    LGraph *lg = global_name2lgraph[path][name];
    I(global_instances.find(path) != global_instances.end());
    I(get_lgid(name) != 0);

    return lg;
  }

  return nullptr;
}

Sub_node &Graph_library::setup_sub(std::string_view name) {
  Lg_type_id lgid = get_lgid(name);
  if (lgid) {
    return attribute[lgid].sub_node;
  }

  lgid = add_name(name,"-");
  I(lgid);
  return attribute[lgid].sub_node;
}

Lg_type_id Graph_library::add_name(std::string_view name, std::string_view source) {
  I(source != "");

  Lg_type_id id = try_get_recycled_id();
  if (id == 0) {
    id = attribute.size();
    attribute.emplace_back();
  } else {
    I(id < attribute.size());
  }

  attribute[id].sub_node.setup(name, id);
  attribute[id].source  = source;
  attribute[id].version = max_next_version.value++;

  graph_library_clean = false;

  I(name2id.find(name) == name2id.end());
  I(id);
  name2id[name] = id;

  return id;
}

std::string Graph_library::get_lgraph_filename(std::string_view path, std::string_view name, std::string_view ext) {
  std::string f;

  f.append(path);
  f.append("/lgraph_");
  f.append(name);
  f.append("_");
  f.append(ext);

  return f;
}

bool Graph_library::rename_name(std::string_view orig, std::string_view dest) {
  auto it = name2id.find(orig);
  if (it == name2id.end()) {
    Pass::error("graph_library: file to rename {} does not exit", orig);
    return false;
  }
  Lg_type_id id = it->second;

  auto dest_it = name2id.find(dest);
  if (dest_it != name2id.end()) {
    auto it2 = global_name2lgraph[path].find(dest);
    I(it2 != global_name2lgraph[path].end());
    expunge_lgraph(dest, it2->second);
  }

  auto it2 = global_name2lgraph[path].find(orig);
  if (it2 != global_name2lgraph[path].end()) {  // orig around, but not open
    global_name2lgraph[path].erase(it2);
  }
  name2id.erase(it);
  I(name2id.find(orig) == name2id.end());

  graph_library_clean    = false;
  attribute[id].sub_node.rename(dest);
  I(attribute[id].sub_node.get_lgid() == id);

  name2id[dest] = id;

  clean_library();

  return true;
}

void Graph_library::update(Lg_type_id lgid) {
  I(lgid < attribute.size());

  if (attribute[lgid].version == (max_next_version - 1)) return;

  graph_library_clean     = false;
  attribute[lgid].version = max_next_version.value++;
}

void Graph_library::update_nentries(Lg_type_id lgid, uint64_t nentries) {
  I(lgid < attribute.size());

  if (attribute[lgid].nentries != nentries) {
    graph_library_clean      = false;
    attribute[lgid].nentries = nentries;
  }
}

void Graph_library::reload() {
  I(graph_library_clean);

  max_next_version = 1;
  std::ifstream graph_list;

  // FIXME: BEGIN DELETE THIS and replace with json reload

  liberty_list.push_back("fake_bad.lib"); // FIXME
  sdc_list.push_back("fake_bad.sdc"); // FIXME
  spef_list.push_back("fake_bad.spef"); // FIXME

  name2id.clear();
  attribute.resize(1);  // 0 is not a valid ID

  if (access(library_file.c_str(), F_OK) != -1) {
    mkdir(path.c_str(), 0755);  // At least make sure directory exists for future
    return;
  }
  FILE *pFile = fopen(library_file.c_str(), "rb");
  if (pFile==0) {
    Pass::error("graph_library::reload could not open graph {} file",library_file);
    return;
  }
  char                      buffer[65536];
  rapidjson::FileReadStream is(pFile, buffer, sizeof(buffer));
  rapidjson::Document       document;
  document.ParseStream<0, rapidjson::UTF8<>, rapidjson::FileReadStream>(is);

  if(document.HasParseError()) {
    Pass::error("graph_library::reload {} Error(offset {}): {}"
        ,library_file
        ,static_cast<unsigned>(document.GetErrorOffset())
        ,rapidjson::GetParseError_En(document.GetParseError()));
    return;
  }

  I(document.HasMember("lgraph"));
  const rapidjson::Value &lgraph_array = document["lgraph"];
  I(lgraph_array.IsArray());
  for(const auto &lg_entry : lgraph_array.GetArray()) {
    I(lg_entry.IsObject());

    I(lg_entry.HasMember("id"));
    I(lg_entry.HasMember("nentries"));
    I(lg_entry.HasMember("source"));
    I(lg_entry.HasMember("version"));

    uint64_t id = lg_entry["id"].GetUint64();;
    if (id>attribute.size())
      attribute.resize(id);

    attribute[id].nentries = lg_entry["nentries"].GetUint64();;
    attribute[id].source   = lg_entry["source"].GetString();;
    attribute[id].version  = lg_entry["version"].GetUint64();;

    if (graph_version.value != 0) {
      attribute[id].sub_node.from_json(lg_entry);

      // NOTE: must use attribute to keep the string in memory
      name2id[sub_node.get_name()] = id;

    } else {
      recycled_id.set_bit(graph_id);
    }

    I(sub_node.get_lgid() == id); // for consistency
  }

#ifndef NDEBUG
  DIR *dir = opendir(path.c_str());
  if (!dir) {
    Pass::error("graph_library.reload: could not open {} directory", path);
    return;
  }
  struct dirent *dent;

  std::set<std::string> lg_found;

  while ((dent = readdir(dir)) != nullptr) {
    if (dent->d_type != DT_REG)  // Only regular files
      continue;
    if (strncmp(dent->d_name, "lgraph_", 7) != 0)  // only if starts with lgraph_
      continue;
    int len = strlen(dent->d_name);
    if (len <= (7 + 6)) continue;
    if (strcmp(dent->d_name + len - 6, "_nodes") != 0)  // and finish with _nodes
      continue;

    const std::string id_str(&dent->d_name[7], len - 7 - 6);
    I(lg_found.find(id_str) == lg_found.end());
    lg_found.insert(id_str);

    bool found = false;
    Lg_type_id id_val = std::stoi(id_str);
    I(id_val>0);
    for (const auto &[name, id] : name2id) {
      if (id_val == id) {
        found = true;
      }
    }
    if (!found) {
      Pass::error("graph_library: directory has id:{} but the {} graph_library does not have it", id_val.value, path);
    }
  }
  closedir(dir);

  for (const auto &[name, id] : name2id) {
    // std::string s(name);
    std::string s = std::to_string(id);
    if (lg_found.find(s) == lg_found.end()) {
      for (auto contents : lg_found) {
        if (contents == s)
          fmt::print("   0lg_found has [{}] [{}]\n", contents, s);
        else
          fmt::print("   1lg_found has [{}] [{}] s:{} s:{} l:{} l:{}\n", contents, s, contents.size(), s.size(), contents.length(),
                     s.length());
      }
      Pass::error("graph_library has id:{} name:{} but the {} directory does not have it", id, name, path);
    }
  }
#endif
}

Graph_library::Graph_library(std::string_view _path) : path(_path), library_file(path + "/" + "graph_library.json") {
  graph_library_clean = true;
  reload();
}

Lg_type_id Graph_library::try_get_recycled_id() {
  if (recycled_id.none()) return 0;

  Lg_type_id lgid = recycled_id.get_first();
  I(lgid <= attribute.size());
  recycled_id.clear(lgid);

  return lgid;
}

void Graph_library::recycle_id(Lg_type_id lgid) {
  if (lgid < attribute.size()) {
    recycled_id.set_bit(lgid);
    return;
  }

  size_t end_pos = attribute.size();
  while (attribute.back().version == 0) {
    attribute.pop_back();
    if (attribute.empty()) break;
  }

  recycled_id.set_range(attribute.size(), end_pos, false);
}

void Graph_library::expunge(std::string_view name) {
  auto it = global_name2lgraph[path].find(name);
  if (it != global_name2lgraph[path].end()) {
    global_name2lgraph[path].erase(it);
  }

  if (attribute[id].nopen != 0) {
    lg->sync();
    // FIXME: Memory leak? it is out there used by someone
  }

  attribute[id].clear();
  recycle_id(id);

  DIR *dr = opendir(path.c_str());
  if (dr == NULL) {
    Pass::error("graph_library: unable to access path {}", path);
    return false;
  }

  struct dirent *de;  // Pointer for directory entry
  std::string match = absl::StrCat("lgraph_", std::to_string(id));
  while ((de = readdir(dr)) != NULL) {
    std::string chop_name(de->d_name, match.size());
    if (chop_name == match) {
      std::string file = absl::StrCat(path,"/",de->d_name);
      fmt::print("deleting... {}\n", file);
      unlink(file.c_str());
    }
  }

  closedir(dr);

  return true;
}

Lg_type_id Graph_library::copy_lgraph(std::string_view name, std::string_view new_name) {
  graph_library_clean = false;
  auto it2 = global_name2lgraph[path].find(name);
  if (it2 != global_name2lgraph[path].end()) {  // orig around, but not open
    it2->second->sync();
  }
  const auto &it = name2id.find(name);
  I(it != name2id.end());
  auto id_orig = it->second;

  Lg_type_id id_new = reset_id(new_name, attribute[id_orig].source);

  attribute[id_new] = attribute[id_orig];
  attribute[id_new].name = new_name;

  DIR *dr = opendir(path.c_str());
  if (dr == NULL) {
    Pass::error("graph_library: unable to access path {}", path);
    return false;
  }

  struct dirent *de;  // Pointer for directory entry
  std::string match = absl::StrCat("lgraph_", std::to_string(id_orig));
  std::string rematch = absl::StrCat("lgraph_", std::to_string(id_new));
  while ((de = readdir(dr)) != NULL) {
    std::string chop_name(de->d_name, match.size());
    if (chop_name == match) {
      std::string_view dname(de->d_name);
      std::string file = absl::StrCat(path,"/",dname);
      std::string_view extension = dname.substr(match.size());

      auto new_file = absl::StrCat(path, "/", rematch, extension);

      fmt::print("copying... {} to {}\n", file, new_file);

      int source = open(file.c_str(), O_RDONLY, 0);
      int dest = open(new_file.c_str(), O_WRONLY | O_CREAT /*| O_TRUNC/**/, 0644);

      // struct required, rationale: function stat() exists also
      struct stat stat_source;
      fstat(source, &stat_source);

      sendfile(dest, source, 0, stat_source.st_size);

      close(source);
      close(dest);
    }
  }

  closedir(dr);

  clean_library();

  return id_new;
}

Lg_type_id Graph_library::register_lgraph(std::string_view name, std::string_view source, LGraph *lg) {
  I(global_name2lgraph[path].find(name) == global_name2lgraph[path].end());

  global_name2lgraph[path][name] = lg;

  Lg_type_id id = reset_id(name, source);

  const auto &it = name2id.find(name);
  I(it != name2id.end());
  attribute[id].nopen++;
  I(attribute[id].sub_node.get_name() == name);

  return id;
}

void Graph_library::unregister(std::string_view name, Lg_type_id lgid) {
  I(attribute.size() > (size_t)lgid);

  I(attribute[lgid].nopen);

  attribute[lgid].nopen--;
  if (attribute[lgid].nopen == 0) {
    if (attribute[lgid].sub_node.is_invalid())
      expunge(name);
  }
}

void Graph_library::sync_all() {
  I(false);  // FIXME: to implement, once we migrate lgraph.cpp to core
#if 0
  for(auto &it:global_name2lgraph) {
    for(auto &it2:it.second) {
      it2.second->sync();
    }
  }
#endif
}

void Graph_library::clean_library() {
  if (graph_library_clean) return;

  rapidjson::StringBuffer                          s;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(s);

  writer.StartObject();

  writer.Key("lgraph");
  writer.StartArray();
  for (size_t id = 1; id < attribute.size(); ++id) {
    const auto &it = attribute[id];
    writer.StartObject();

    writer.Key("id");
    writer.Uint64(id);

    writer.Key("version");
    writer.Uint64(it.version);

    writer.Key("nentries");
    writer.Uint64(it.nentries);

    writer.Key("source");
    writer.String(it.source.c_str());

    it.sub_node.to_json(writer);

    writer.EndObject();
  }
  writer.EndArray();

  writer.Key("liberty");
  writer.StartArray();
  for (const auto lib:liberty_list) {
    writer.StartObject();

    writer.Key("file");
    writer.String(lib.c_str());

    writer.EndObject();
  }
  writer.EndArray();

  writer.Key("sdc");
  writer.StartArray();
  for (const auto lib:sdc_list) {
    writer.StartObject();

    writer.Key("file");
    writer.String(lib.c_str());

    writer.EndObject();
  }
  writer.EndArray();

  writer.Key("spef");
  writer.StartArray();
  for (const auto lib:spef_list) {
    writer.StartObject();

    writer.Key("file");
    writer.String(lib.c_str());

    writer.EndObject();
  }
  writer.EndArray();
  writer.EndObject();

  {
    std::ofstream fs;

    fs.open(library_file, std::ios::out | std::ios::trunc);
    if(!fs.is_open()) {
      Pass::error("ERROR: could not open graph_library file {}", library_file);
      return;
    }
    fs << s.GetString() << std::endl;
    fs.close();
  }

  graph_library_clean = true;
}

void Graph_library::each_type(std::function<void(Lg_type_id, std::string_view)> f1) const {
  for (const auto [name, id] : name2id) {
    f1(id, name);
  }
}
void Graph_library::each_type(std::function<bool(Lg_type_id, std::string_view)> f1) const {
  for (const auto [name, id] : name2id) {
    bool cont = f1(id, name);
    if (!cont) break;
  }
}

void Graph_library::each_type(std::string_view match, std::function<void(Lg_type_id, std::string_view)> f1) const {
  const std::string string_match(match);  // NOTE: regex does not support string_view, c++20 may fix this missing feature
  const std::regex  txt_regex(string_match);

  for (const auto [name, id] : name2id) {
    const std::string line(name);
    if (!std::regex_search(line, txt_regex)) continue;

    f1(id, name);
  }
}

void Graph_library::each_type(std::string_view match, std::function<bool(Lg_type_id, std::string_view)> f1) const {
  const std::string string_match(match);  // NOTE: regex does not support string_view, c++20 may fix this missing feature
  const std::regex  txt_regex(string_match);

  for (const auto [name, id] : name2id) {
    const std::string line(name);
    if (!std::regex_search(line, txt_regex)) continue;

    bool cont = f1(id, name);
    if (!cont) break;
  }
}
