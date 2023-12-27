#include "logger.hpp"

#include "abconfig.h"
#include "abnativeelf.hpp"
#include "bashinterface.hpp"
#include "pm.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <unistd.h>
#include <unordered_set>

using json = nlohmann::json;

extern "C" {
#include "abnativefunctions.h"
#include "bashincludes.h"
extern volatile int last_command_exit_value;
}

#ifdef ALT_ARRAY_IMPLEMENTATION
#error Bash compiled with ALT_ARRAY_IMPLEMENTATION is not supported
#endif

static bool set_registered_flag() {
  if (find_variable("__ABNR"))
    return true;
  auto *flag_var =
      bind_global_variable("__ABNR", const_cast<char *>("__ABNR"), ASS_FORCE);
  flag_var->attributes |= att_invisible | att_nounset | att_readonly;
  return false;
}

static inline char *get_argv1(WORD_LIST *list) {
  if (!list)
    return nullptr;
  const WORD_DESC *wd = list->word;
  if (!wd)
    return nullptr;
  return wd->word;
}

static inline std::string get_all_args(WORD_LIST *list) {
  if (!list)
    return std::string();
  std::string args{};
  args.reserve(16);
  while (list) {
    args += get_argv1(list);
    if (list->next)
      args += " ";
    list = list->next;
  }
  return args;
}

static inline std::string get_self_path() {
  auto *path_var = find_variable("AB");
  if (!path_var) {
    return {};
  }
  const char *path = path_var->value;
  const std::string ab_path{path};
  return ab_path;
}

static inline std::string arch_findfile_maybe_stage2(std::string &path,
                                                     bool is_stage2) {
  auto test_path_stage2 = path + ".stage2";
  if (is_stage2) {
    if (access(test_path_stage2.c_str(), F_OK) == 0) {
      return path;
    }
    // TODO: show warning if stage2 file not found
    auto *log = reinterpret_cast<BaseLogger *>(logger);
    log->warning(
        "Unable to find stage2 defines, falling back to normal defines ...");
  }
  if (access(path.c_str(), F_OK) == 0) {
    return path;
  }
  return {};
}

static inline std::string arch_findfile_inner(const std::string &path,
                                              bool stage2_aware) {
  const auto defines_path = std::string{"autobuild/"};
  // find arch-specific directory first
  const auto *arch_name = find_variable("ABHOST");
  const auto *stage2_v = find_variable("ABSTAGE2");
  const bool is_stage2 =
      stage2_aware && (stage2_v ? autobuild_bool(stage2_v->value) : false);
  if (arch_name && arch_name->value) {
    auto test_path = defines_path + arch_name->value + "/" + path;
    const auto result = arch_findfile_maybe_stage2(test_path, is_stage2);
    if (!result.empty()) {
      return result;
    }
  }

  // then try the ABHOST_GROUP
  const auto *ag_v = find_variable("ABHOST_GROUP");
  if (!ag_v || !(ag_v->attributes & att_array))
    return "";
  const auto *ag_a = array_cell(ag_v);
  for (ARRAY_ELEMENT *ae = element_forw(ag_a->head); ae != ag_a->head;
       ae = element_forw(ae)) {
    auto test_path = defines_path + ae->value + "/" + path;
    const auto result = arch_findfile_maybe_stage2(test_path, is_stage2);
    // TODO: check if the group name is ambiguous
    if (!result.empty()) {
      return result;
    }
  }

  // lastly, try to find the file in the standard location
  {
    auto test_path = defines_path + path;
    const auto result = arch_findfile_maybe_stage2(test_path, is_stage2);
    if (!result.empty()) {
      return result;
    }
  }
  // not found
  return {};
}

static inline void bash_array_push(ARRAY *array, char *value) {
  auto *new_ae = array_create_element(0, value);
  // new element is the last element of the array (tail of the linked-list)
  new_ae->prev = array->lastref;
  new_ae->next = array->lastref->next;
  array->lastref->next = new_ae;
  array->lastref = new_ae;
  array->num_elements++;
}

static int ab_bool(WORD_LIST *list) {
  char *argv1 = get_argv1(list);
  if (!argv1)
    return 2;
  const int result = autobuild_bool(argv1);
  switch (result) {
  // bash returns 1 for false, 0 for true
  // so we need to reverse the result
  case 0:
    return 1;
  case 1:
    return 0;
  default:
    return 2;
  }
}

static int ab_isarray(WORD_LIST *list) {
  char *argv1 = get_argv1(list);
  if (!argv1)
    return 1;
  const auto *var = find_variable(argv1);
  // the return value is 0 (true in bash terms) if the variable is an array, 1
  // (false in bash terms) otherwise
  if (!var)
    return 1;
  if (var->attributes & att_array)
    return 0;
  return 1;
}

static int ab_isdefined(WORD_LIST *list) {
  char *argv1 = get_argv1(list);
  if (!argv1)
    return 1;
  const auto *var = find_variable(argv1);
  // the return value is 0 (true in bash terms) if the variable is an array, 1
  // (false in bash terms) otherwise
  // if the variable is invisible, then it's undefined as well
  if (!var || var->attributes & att_invisible)
    return 1;
  return 0;
}

static int ab_load_strict(WORD_LIST *list) {
  char *argv1 = get_argv1(list);
  if (!argv1)
    return 1;
  const int result = autobuild_load_file(argv1, true);
  if (result)
    return result;
  return autobuild_load_file(argv1, false);
}

static int ab_print_backtrace(WORD_LIST *list) {
  if (last_command_exit_value != 0) {
    autobuild_get_backtrace();
    return last_command_exit_value;
  }
  return 0;
}

int setup_default_env_variables() {
  if (!getenv("SHELL")) {
    char bash_path[4096]{};
    if (!readlink("/proc/self/exe", bash_path, 4096)) {
      assert("Failed to readlink /proc/self/exe");
    }
    setenv("SHELL", bash_path, true);
  }

  // TODO: set the correct HOME variable

  setenv("TZ", "UTC", true);

  return 0;
}

int ab_filter_args(WORD_LIST *list) {
  char *argv1 = get_argv1(list);
  if (!argv1)
    return 1;
  auto to_remove = std::unordered_set<std::string>{};
  to_remove.reserve(8);
  // start with the second argument
  for (WORD_LIST *list_iter = list->next; list_iter;
       list_iter = list_iter->next) {
    char *pattern = get_argv1(list_iter);
    if (!pattern)
      continue;
    to_remove.emplace(pattern);
  }
  const auto *var = find_variable(argv1);
  // the return value is 0 (true in bash terms) if the variable is an array, 1
  // (false in bash terms) otherwise
  if (!var)
    return 1;
  if (!array_p(var))
    return 1;
  auto *var_a = array_cell(var);
  for (ARRAY_ELEMENT *ae = element_forw(var_a->head); ae != var_a->head;
       ae = element_forw(ae)) {
    if (to_remove.find(ae->value) == to_remove.end())
      continue;

    // delete the element from the array (double-linked-list)
    ae->prev->next = ae->next;
    ae->next->prev = ae->prev;
    var_a->num_elements--;

    if (ae->next != var_a->head) {
      var_a->lastref = ae->next;
    } else if (ae->prev != var_a->head) {
      var_a->lastref = ae->prev;
    } else {
      // invalidate the lastref pointer
      var_a->lastref = nullptr;
    }
    array_dispose_element(ae);
  }
  return 0;
}

constexpr const char *ab_get_current_architecture() {
  static_assert(native_arch_name && native_arch_name[0],
                "native_arch_name is undefined or empty");
  return native_arch_name;
}

static int abinfo(WORD_LIST *list) {
  const auto message = get_all_args(list);
  auto *log = reinterpret_cast<BaseLogger *>(logger);
  log->info(message);
  return 0;
}

static int abwarn(WORD_LIST *list) {
  const auto message = get_all_args(list);
  auto *log = reinterpret_cast<BaseLogger *>(logger);
  log->warning(message);
  return 0;
}

static int aberr(WORD_LIST *list) {
  const auto message = get_all_args(list);
  auto *log = reinterpret_cast<BaseLogger *>(logger);
  log->error(message);
  return 0;
}

static int abdbg(WORD_LIST *list) {
  const auto message = get_all_args(list);
  auto *log = reinterpret_cast<BaseLogger *>(logger);
  log->debug(message);
  return 0;
}

static int abdie(WORD_LIST *list) {
  const auto message = get_argv1(list);
  const auto exit_value = list ? get_argv1(list->next) : nullptr;
  const int exit_code =
      exit_value ? std::atoi(exit_value) : last_command_exit_value;

  auto *log = reinterpret_cast<BaseLogger *>(logger);

  const auto diag = autobuild_get_backtrace();
  log->logDiagnostic(diag);
  log->logException(message ? message : std::string());

  // reset the traps to avoid double-triggering
  autobuild_switch_strict_mode(false);
  exit_shell(exit_code);
  return exit_code;
}

static void register_logger_from_env() {
  auto *var = find_variable_tempenv("ABREPORTER");
  if (!var || !var->value) {
    logger = reinterpret_cast<Logger *>(new PlainLogger());
    return;
  }
  const char *reporter = var->value;
  if (strncmp(reporter, "color", 5) == 0) {
    logger = reinterpret_cast<Logger *>(new ColorfulLogger());
    return;
  } else if (strncmp(reporter, "json", 4) == 0) {
    logger = reinterpret_cast<Logger *>(new JsonLogger());
    return;
  }
  logger = reinterpret_cast<Logger *>(new PlainLogger());
}

static int set_arch_variables() {
  const std::string ab_path = get_self_path();
  if (ab_path.empty()) {
    return 1;
  }
  // read targets
  const auto arch_targets_path = ab_path + "/sets/arch_targets.json";
  std::ifstream targets_file(arch_targets_path);
  json arch_targets = json::parse(targets_file);
  auto *arch_target_var =
      make_new_assoc_variable(const_cast<char *>("ARCH_TARGET"));
  auto *arch_target_var_h = assoc_cell(arch_target_var);
  for (json::iterator it = arch_targets.begin(); it != arch_targets.end();
       ++it) {
    assoc_insert(
        arch_target_var_h, const_cast<char *>(it.key().c_str()),
        const_cast<char *>(it.value().template get<std::string>().c_str()));
  }

  // set ARCH variables
  constexpr const auto this_arch = ab_get_current_architecture();
  auto *arch_v = bind_variable("ARCH", const_cast<char *>("ARCH"), 0);
  // ARCH=$(abdetectarch)
  arch_v->value = strdup(this_arch);
  auto *host_v = bind_variable("ABHOST", const_cast<char *>("ABHOST"), 0);
  // ABHOST=ARCH
  host_v->value = strdup(this_arch);
  auto *build_v = bind_variable("ABBUILD", const_cast<char *>("ABBUILD"), 0);
  // ABBUILD=ARCH
  build_v->value = strdup(this_arch);

  const std::string arch_triple =
      arch_targets[this_arch].template get<std::string>();
  // set HOST
  auto *host_var = bind_variable("HOST", const_cast<char *>("HOST"), 0);
  // HOST=${ARCH_TARGET["$ABHOST"]}
  host_var->value = strdup(arch_triple.c_str());
  // set BUILD
  auto *build_var = bind_variable("BUILD", const_cast<char *>("BUILD"), 0);
  // BUILD=${ARCH_TARGET["$ABBUILD"]}
  build_var->value = strdup(arch_triple.c_str());

  // set ABHOST_GROUP
  auto *arch_groups_v =
      make_new_array_variable(const_cast<char *>("ABHOST_GROUP"));
  auto *arch_groups_a = array_cell(arch_groups_v);
  const auto arch_groups_path = ab_path + "/sets/arch_groups.json";
  std::ifstream groups_file(arch_groups_path);
  json arch_groups = json::parse(groups_file);
  for (json::iterator it = arch_groups.begin(); it != arch_groups.end(); ++it) {
    const auto group_name = it.key();
    auto group = it.value();
    // search inner array for the target architecture name
    for (json::iterator it = group.begin(); it != group.end(); ++it) {
      const auto arch_value = it.value().template get<std::string>();
      if (arch_value == this_arch) {
        // append the group name to the array
        bash_array_push(arch_groups_a, const_cast<char *>(group_name.c_str()));
      }
    }
  }

  return 0;
}

static inline int arch_loadvar_inner(const std::string &var_name) {
  std::vector<std::string> aliases{};
  aliases.reserve(4);
  const auto arch_v = find_variable("ARCH");
  // ARCH can't be an array
  if (!arch_v || arch_v->attributes & att_array)
    return 1;
  const auto ag_v = find_variable("ABHOST_GROUP");
  // ABHOST_GROUP needs to be an array
  if (!ag_v || !(ag_v->attributes & att_array))
    return 1;
  aliases.emplace_back(arch_v->value);
  const auto ag_a = array_cell(ag_v);
  for (ARRAY_ELEMENT *ae = element_forw(ag_a->head); ae != ag_a->head;
       ae = element_forw(ae)) {
    aliases.emplace_back(ae->value);
  }

  const int result = autobuild_get_variable_with_suffix(var_name, aliases);
  if (result != 0) {
    // auto *log = reinterpret_cast<BaseLogger *>(logger);
    // TODO: log: Refusing to assign [...]
    return 1;
  }
  return 0;
}

static int arch_loadvar(WORD_LIST *list) {
  const auto name = get_argv1(list);
  if (!name)
    return 0;
  arch_loadvar_inner(name);

  return 0;
}

static int arch_loaddefines(WORD_LIST *list) {
  // parse args
  int opt = 0;
  bool stage2_aware = false;
  reset_internal_getopt();
  while ((opt = internal_getopt(list, const_cast<char *>("2"))) != -1) {
    switch (opt) {
    case '2':
      stage2_aware = true;
      break;
    default:
      return 1;
    }
  }
  const auto *argv1 = get_argv1(loptend);
  if (!argv1)
    return 1;

  // load exported variables list
  const std::string ab_path = get_self_path();
  if (ab_path.empty()) {
    return 1;
  }
  const std::string exported_vars_path = ab_path + "/sets/exports.json";
  std::ifstream exported_vars_file(exported_vars_path);
  json exported_vars_json = json::parse(exported_vars_file);
  std::vector<std::string> exported_vars;
  exported_vars.reserve(64);
  for (json::iterator it = exported_vars_json.begin();
       it != exported_vars_json.end(); ++it) {
    const auto inner_array =
        it.value().template get<std::vector<std::string>>();
    exported_vars.insert(exported_vars.end(), inner_array.begin(),
                         inner_array.end());
  }

  // load the defines file
  const auto defines_path = arch_findfile_inner(argv1, stage2_aware);
  if (defines_path.empty())
    return 127;
  const int result = autobuild_load_file(defines_path.c_str(), false);
  if (result != 0)
    return result;
  // load the actual variables from the current context
  for (const auto &var : exported_vars) {
    arch_loadvar_inner(var);
  }
  return 0;
}

static int arch_loadfile_strict(WORD_LIST *list) {
  // parse args
  int opt = 0;
  bool stage2_aware = false;
  reset_internal_getopt();
  while ((opt = internal_getopt(list, const_cast<char *>("2"))) != -1) {
    switch (opt) {
    case '2':
      stage2_aware = true;
      break;
    default:
      return 1;
    }
  }
  const auto *argv1 = get_argv1(loptend);
  if (!argv1)
    return 1;
  const auto filepath = arch_findfile_inner(argv1, stage2_aware);
  if (filepath.empty())
    return 127;
  return autobuild_load_file(filepath.c_str(), false);
}

static int arch_loadfile(WORD_LIST *list) {
  auto *log = reinterpret_cast<BaseLogger *>(logger);
  log->warning(
      "arch_loadfile is deprecated. Use arch_loadfile_strict instead.");
  return arch_loadfile_strict(list);
}

static int ab_read_listing_file(WORD_LIST *list) {
  const auto *filename = get_argv1(list);
  if (!filename)
    return 1;
  auto *result_varname = get_argv1(list->next);
  if (!result_varname)
    return 1;

  auto file = std::ifstream(filename);
  if (!file.is_open())
    return 1;
  std::string line;
  std::vector<std::string> lines{};
  while (std::getline(file, line)) {
    // TODO: trim whitespaces
    if (line.front() == '#')
      continue;
    lines.emplace_back(line);
  }
  file.close();
  const auto *result_var = make_new_array_variable(result_varname);
  auto *result_var_a = array_cell(result_var);
  for (const auto &line : lines) {
    bash_array_push(result_var_a, const_cast<char *>(line.c_str()));
  }
  return 0;
}

static int ab_tostringarray(WORD_LIST *list) {
  const auto *varname = get_argv1(list);
  if (!varname)
    return 1;

  auto *result_var = find_variable(varname);
  if (!result_var)
    return 1;
  // already an array, nothing to do
  if (result_var->attributes & att_array)
    return 0;
  // FIXME: does not work for assoc vars
  if (result_var->attributes & att_assoc)
    return 4;

  // convert the string to an array
  // save the string value first
  auto oldval =
      std::unique_ptr<char, decltype(&free)>(strdup(result_var->value), &free);
  free(result_var->value);
  // override the value to an array
  result_var->value = reinterpret_cast<char *>(array_create());
  // override the attributes to be an array type
  result_var->attributes |= att_array;
  // split the string into array elements
  assign_array_var_from_string(result_var, oldval.get(), 0);
  return 0;
}

static int abcopyvar(WORD_LIST *list) {
  const auto *src = get_argv1(list);
  if (!src)
    return 1;
  const auto *dst = get_argv1(list->next);
  if (!dst)
    return 1;
  // TODO: handle errors
  return autobuild_copy_variable_value(src, dst);
}

static int arch_findfile(WORD_LIST *list) {
  // parse args
  int opt = 0;
  bool stage2_aware = false;
  reset_internal_getopt();
  while ((opt = internal_getopt(list, const_cast<char *>("2"))) != -1) {
    switch (opt) {
    case '2':
      stage2_aware = true;
      break;
    default:
      return 1;
    }
  }
  const auto *argv1 = get_argv1(loptend);
  if (!argv1)
    return 1;
  const auto filepath = arch_findfile_inner(argv1, stage2_aware);
  if (filepath.empty())
    return 127;
  std::cout << filepath << std::endl;
  return 0;
}

static int ab_concatarray(WORD_LIST *list) {
  const auto *dst = get_argv1(list);
  if (!dst)
    return 1;
  const auto *dst_v = find_variable(dst);
  if (!dst_v || !(dst_v->attributes & att_array))
    return 1;
  const auto *src = get_argv1(list->next);
  if (!src)
    return 1;
  const auto *src_v = find_variable(src);
  if (!src_v || !(src_v->attributes & att_array))
    return 1;
  auto *dst_a = array_cell(dst_v);
  const auto *src_a = array_cell(src_v);
  for (ARRAY_ELEMENT *ae = element_forw(src_a->head); ae != src_a->head;
       ae = element_forw(ae)) {
    bash_array_push(dst_a, ae->value);
  }
  return 0;
}

static int ab_typecheck(WORD_LIST *list) {
  int opt{};
  int expected_type = 0;
  reset_internal_getopt();
  while ((opt = internal_getopt(list, const_cast<char *>("ahsif")))) {
    switch (opt) {
    case 'a':
      expected_type |= att_array;
      break;
    case 'h':
      expected_type |= att_assoc;
      break;
    case 's':
      expected_type |= 0;
      break;
    case 'i':
      expected_type |= att_integer;
      break;
    case 'f':
      expected_type |= att_function;
      break;
    default:
      return 1;
    }
  }

  const auto *varname = get_argv1(loptend);
  if (!varname)
    return 1;
  const auto *var = find_variable(varname);
  if (!var)
    return 1;
  if (var->attributes & expected_type)
    return 0;
  return 1;
}

static int abpm_dump_builddep_req(WORD_LIST *list) {
  std::mt19937_64 rng(std::random_device{}());
  std::cout << "Source: "
            << "ab4-satdep-" << std::to_string(rng()) << "\nBuild-Depends:\n";
  for (; list != nullptr; list = list->next) {
    const auto converted = autobuild_to_deb_version(list->word->word);
    if (converted.empty()) {
      return 1;
    }
    std::cout << " " << converted << ",\n";
  }
  return 0;
}

static int abpm_genver(WORD_LIST *list) {
  const auto argv1 = get_argv1(list);
  if (!argv1)
    return 1;
  std::cout << autobuild_to_deb_version(argv1) << std::endl;
  return 0;
}

static int abelf_elf_copy_to_symdir(WORD_LIST *list) {
  const auto *src = get_argv1(list);
  if (!src)
    return 1;
  WORD_LIST *lists = list->next;
  const auto *buildid = get_argv1(lists);
  if (!buildid)
    return 1;
  lists = lists->next;
  const auto *symdir = get_argv1(lists);
  if (!symdir)
    return 1;

  return elf_copy_to_symdir(src, symdir, buildid);
}

static int abelf_copy_dbg(WORD_LIST *list) {
  const auto *src = get_argv1(list);
  if (!src)
    return 1;
  WORD_LIST *lists = list->next;
  const auto *dst = get_argv1(lists);
  if (!dst)
    return 1;
  int ret = elf_copy_debug_symbols(src, dst, false, true);
  if (ret < 0)
    return 10;
  return 0;
}

extern "C" {
void register_all_native_functions() {
  if (set_registered_flag())
    return;
  const std::unordered_map<const char *, builtin_func_t> functions{
      {"bool", ab_bool},
      {"abisarray", ab_isarray},
      {"abisdefined", ab_isdefined},
      {"load_strict", ab_load_strict},
      {"diag_print_backtrace", ab_print_backtrace},
      // previously in base.sh
      {"abinfo", abinfo},
      {"abwarn", abwarn},
      {"aberr", aberr},
      {"abdbg", abdbg},
      {"abdie", abdie},
      // previously in arch.sh
      {"arch_loadvar", arch_loadvar},
      {"arch_loaddefines", arch_loaddefines},
      {"arch_loadfile", arch_loadfile},
      {"arch_loadfile_strict", arch_loadfile_strict},
      {"arch_findfile", arch_findfile},
      {"abcopyvar", abcopyvar},
      {"ab_concatarray", ab_concatarray},
      // previously in elf.sh
      {"elf_install_symfile", abelf_elf_copy_to_symdir},
      {"elf_copydbg", abelf_copy_dbg},
      // new stuff
      {"ab_filter_args", ab_filter_args},
      {"ab_read_listing_file", ab_read_listing_file},
      {"ab_tostringarray", ab_tostringarray},
      {"ab_typecheck", ab_typecheck},
      {"abpm_debver", abpm_genver},
      {"abpm_dump_builddep_req", abpm_dump_builddep_req},
  };

  // Initialize logger
  if (!logger)
    register_logger_from_env();

  autobuild_register_builtins(functions);
}

void register_builtin_variables() {
  // TODO: error handling
  setup_default_env_variables();
  set_arch_variables();
}
} // extern "C"
