extern "C" {
#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_path.h"
#include "sentry_utils.h"
}

#include <map>
#include <vector>

#ifdef __GNUC__
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#    pragma GCC diagnostic ignored "-Wfour-char-constants"
#endif

#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/settings.h"

#ifdef __GNUC__
#    pragma GCC diagnostic pop
#endif

extern "C" {

typedef struct {
    sentry_path_t *event_path;
    sentry_path_t *breadcrumb1_path;
    sentry_path_t *breadcrumb2_path;
    size_t num_breadcrumbs;
} crashpad_state_t;

static std::unique_ptr<crashpad::CrashReportDatabase> g_db;

static void
sentry__crashpad_backend_user_consent_changed(sentry_backend_t *UNUSED(backend))
{
    if (!g_db || !g_db->GetSettings()) {
        return;
    }
    g_db->GetSettings()->SetUploadsEnabled(!sentry__should_skip_upload());
}

static void
sentry__crashpad_backend_shutdown(sentry_backend_t *UNUSED(backend))
{
    g_db = nullptr;
}

static void
sentry__crashpad_backend_startup(sentry_backend_t *backend)
{
    // TODO: backends should really get the options as argument
    const sentry_options_t *options = sentry_get_options();

    sentry_path_t *owned_handler_path = NULL;
    sentry_path_t *handler_path = options->handler_path;
    if (!handler_path) {
        sentry_path_t *current_exe = sentry__path_current_exe();
        if (current_exe) {
            sentry_path_t *exe_dir = sentry__path_dir(current_exe);
            sentry__path_free(current_exe);
            if (exe_dir) {
                handler_path = sentry__path_join_str(exe_dir,
#ifdef SENTRY_PLATFORM_WINDOWS
                    "crashpad_handler.exe"
#else
                    "crashpad_handler"
#endif
                );
                owned_handler_path = handler_path;
                sentry__path_free(exe_dir);
            }
        }
    }

    if (!handler_path || !sentry__path_is_file(handler_path)) {
        SENTRY_DEBUG("unable to start crashpad backend, invalid handler_path");
        sentry__path_free(owned_handler_path);
        return;
    }

    SENTRY_TRACE("starting crashpad backend");
    sentry_path_t *current_run_folder = options->run->run_path;
    crashpad_state_t *data = (crashpad_state_t *)backend->data;

    base::FilePath database(options->database_path->path);
    base::FilePath handler(handler_path->path);
    sentry__path_free(owned_handler_path);

    std::map<std::string, std::string> annotations;
    std::map<std::string, base::FilePath> file_attachments;

    // register attachments
    for (sentry_attachment_t *attachment = options->attachments; attachment;
         attachment = attachment->next) {
        file_attachments.emplace(
            attachment->name, base::FilePath(attachment->path->path));
    }

    // and add the serialized event, and two rotating breadcrumb files
    // as attachments and make sure the files exist
    data->event_path
        = sentry__path_join_str(current_run_folder, "__sentry-event");
    data->breadcrumb1_path
        = sentry__path_join_str(current_run_folder, "__sentry-breadcrumb1");
    data->breadcrumb2_path
        = sentry__path_join_str(current_run_folder, "__sentry-breadcrumb2");

    sentry__path_touch(data->event_path);
    sentry__path_touch(data->breadcrumb1_path);
    sentry__path_touch(data->breadcrumb2_path);

    file_attachments.emplace(
        "__sentry-event", base::FilePath(data->event_path->path));
    file_attachments.emplace(
        "__sentry-breadcrumb1", base::FilePath(data->breadcrumb1_path->path));
    file_attachments.emplace(
        "__sentry-breadcrumb2", base::FilePath(data->breadcrumb2_path->path));

    std::vector<std::string> arguments;
    arguments.push_back("--no-rate-limit");

#ifdef _WIN32
    // Temporary fix for Windows
    arguments.push_back("--no-upload-gzip");
#endif

    // initialize database first and check for user consent.  This is going
    // to change the setting persisted into the crashpad database.  The
    // update to the consent change is then reflected when the handler starts.
    g_db = crashpad::CrashReportDatabase::Initialize(database);
    sentry__crashpad_backend_user_consent_changed(backend);

    crashpad::CrashpadClient client;
    char *minidump_url = sentry__dsn_get_minidump_url(&options->dsn);
    std::string url = minidump_url ? std::string(minidump_url) : std::string();
    sentry_free(minidump_url);
    bool success = client.StartHandlerWithAttachments(handler, database,
        database, url, annotations, file_attachments, arguments,
        /* restartable */ true,
        /* asynchronous_start */ false);

    if (success) {
        SENTRY_DEBUG("started crashpad client handler.");
    } else {
        SENTRY_DEBUG("failed to start crashpad client handler.");
        return;
    }

    if (!options->system_crash_reporter_enabled) {
        // Disable the system crash reporter. Especially on macOS, it takes
        // substantial time *after* crashpad has done its job.
        crashpad::CrashpadInfo *crashpad_info
            = crashpad::CrashpadInfo::GetCrashpadInfo();
        crashpad_info->set_system_crash_reporter_forwarding(
            crashpad::TriState::kDisabled);
    }
}

static void
sentry__crashpad_backend_flush_scope(
    sentry_backend_t *backend, const sentry_scope_t *UNUSED(scope))
{
    const crashpad_state_t *data = (crashpad_state_t *)backend->data;
    if (!data->event_path) {
        return;
    }

    // This here is an empty object that we copy the scope into.
    // Even though the API is specific to `event`, an `event` has a few default
    // properties that we do not want here.
    sentry_value_t event = sentry_value_new_object();
    SENTRY_WITH_SCOPE (scope) {
        // we want the scope without any modules or breadcrumbs
        sentry__scope_apply_to_event(scope, event, SENTRY_SCOPE_NONE);
    }

    size_t mpack_size;
    char *mpack = sentry_value_to_msgpack(event, &mpack_size);
    sentry_value_decref(event);
    if (!mpack) {
        return;
    }

    int rv = sentry__path_write_buffer(data->event_path, mpack, mpack_size);
    sentry_free(mpack);

    if (rv != 0) {
        SENTRY_DEBUG("flushing scope to msgpack failed");
    }
}

static void
sentry__crashpad_backend_add_breadcrumb(
    sentry_backend_t *backend, sentry_value_t breadcrumb)
{
    crashpad_state_t *data = (crashpad_state_t *)backend->data;

    bool first_breadcrumb = data->num_breadcrumbs % SENTRY_BREADCRUMBS_MAX == 0;

    const sentry_path_t *breadcrumb_file
        = data->num_breadcrumbs % (SENTRY_BREADCRUMBS_MAX * 2)
            < SENTRY_BREADCRUMBS_MAX
        ? data->breadcrumb1_path
        : data->breadcrumb2_path;
    data->num_breadcrumbs++;
    if (!breadcrumb_file) {
        return;
    }

    size_t mpack_size;
    char *mpack = sentry_value_to_msgpack(breadcrumb, &mpack_size);
    sentry_value_decref(breadcrumb);
    if (!mpack) {
        return;
    }

    int rv = first_breadcrumb
        ? sentry__path_write_buffer(breadcrumb_file, mpack, mpack_size)
        : sentry__path_append_buffer(breadcrumb_file, mpack, mpack_size);
    sentry_free(mpack);

    if (rv != 0) {
        SENTRY_DEBUG("flushing breadcrumb to msgpack failed");
    }
}

static void
sentry__crashpad_backend_free(sentry_backend_t *backend)
{
    crashpad_state_t *data = (crashpad_state_t *)backend->data;
    sentry__path_free(data->event_path);
    sentry__path_free(data->breadcrumb1_path);
    sentry__path_free(data->breadcrumb2_path);
    sentry_free(data);
}

static void
sentry__crashpad_backend_except(
    sentry_backend_t *UNUSED(backend), sentry_ucontext_t *context)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    crashpad::CrashpadClient::DumpAndCrash(&context->exception_ptrs);
#else
    // TODO: Crashpad has the ability to do this on linux / mac but the
    // method interface is not exposed for it, a patch would be required
    (void)context;
#endif
}

sentry_backend_t *
sentry__backend_new(void)
{
    sentry_backend_t *backend = SENTRY_MAKE(sentry_backend_t);
    if (!backend) {
        return NULL;
    }
    crashpad_state_t *data = SENTRY_MAKE(crashpad_state_t);
    if (!data) {
        sentry_free(backend);
        return NULL;
    }
    memset(data, 0, sizeof(crashpad_state_t));

    backend->startup_func = sentry__crashpad_backend_startup;
    backend->shutdown_func = sentry__crashpad_backend_shutdown;
    backend->except_func = sentry__crashpad_backend_except;
    backend->free_func = sentry__crashpad_backend_free;
    backend->flush_scope_func = sentry__crashpad_backend_flush_scope;
    backend->add_breadcrumb_func = sentry__crashpad_backend_add_breadcrumb;
    backend->user_consent_changed_func
        = sentry__crashpad_backend_user_consent_changed;
    backend->data = data;

    return backend;
}
}
