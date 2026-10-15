#include "../phase2/file_access.h"
#include "../phase2/privilege_manager.h"
#include "../phase2/binder_exploit.h"
#include "../phase2/phase2_session.h"

#include <type_traits>

static_assert(!std::is_copy_constructible<phase2::FileHandle>::value, "File ownership must be unique");
static_assert(std::is_nothrow_move_constructible<phase2::FileHandle>::value, "Handles must move safely");
static_assert(!std::is_copy_constructible<phase2::BinderExploitManager>::value, "Device ownership must be unique");
static_assert(!std::is_copy_constructible<phase2::Phase2Session>::value, "Sessions must not be copied");
