#include "logger.hpp"

#include "abconfig.h"
#include "bashinterface.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
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

static int arch_loadfile_strict(WORD_LIST *list) { return 0; }

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
  auto *path_var = find_variable("AB");
  if (!path_var) {
    return 1;
  }
  const char *path = path_var->value;
  const std::string ab_path{path};
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

  const std::string arch_triple = arch_targets[this_arch].template get<std::string>();
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
        array_push(arch_groups_a, const_cast<char *>(group_name.c_str()));
      }
    }
  }

  return 0;
}

static int arch_loadvar(WORD_LIST *list) {
  // TODO
  return 0;
}

static int arch_loadfile(WORD_LIST *list) {
  // TODO
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
      {"ab_filter_args", ab_filter_args},
      {"arch_loadfile_strict", arch_loadfile_strict},
      {"abinfo", abinfo},
      {"abwarn", abwarn},
      {"aberr", aberr},
      {"abdbg", abdbg},
      {"abdie", abdie},
  };
  // puts("Registering all native functions");
  // Initialize logger
  if (!logger)
    register_logger_from_env();
  // autobuild_switch_strict_mode(true);
  autobuild_register_builtins(functions);
}

void register_builtin_variables() {
  // TODO: error handling
  setup_default_env_variables();
  set_arch_variables();
}
} // extern "C"
