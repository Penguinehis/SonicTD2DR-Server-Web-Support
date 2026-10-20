#include <AdminCLI.h>
#include <Lib.h>
#include <Log.h>
#include <io/Threads.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

/* The admin CLI blocks in fgets(). A shutdown signal stops the worker threads
   but cannot unblock that read (glibc restarts it, and Windows runs the handler
   on another thread entirely), so the process would sit there forever. Running
   the CLI on its own thread lets main() wait on the workers instead and exit as
   soon as they stop, whichever way the shutdown was requested. */
#if defined(__unix) || defined(__unix__)
static void *admin_cli_entry(void *arg)
{
    (void)arg;
    admin_cli_run();
    disaster_shutdown();
    return NULL;
}
#else
static DWORD WINAPI admin_cli_entry(LPVOID arg)
{
    (void)arg;
    admin_cli_run();
    disaster_shutdown();
    return 0;
}
#endif

static bool admin_cli_spawn(void)
{
#if defined(__unix) || defined(__unix__)
    pthread_t thread;
    if (pthread_create(&thread, NULL, admin_cli_entry, NULL) != 0)
        return false;
    pthread_detach(thread);
    return true;
#else
    HANDLE thread = CreateThread(NULL, 0, admin_cli_entry, NULL, 0, NULL);
    if (!thread)
        return false;
    CloseHandle(thread);
    return true;
#endif
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--cli|--nogui|--no-cli]\n", program ? program : "DisasterServer");
    puts("  --cli       force the interactive admin CLI even when stdin is redirected");
    puts("  --nogui     legacy alias for --cli (the normal build is already headless)");
    puts("  --no-cli    disable interactive CLI and run until a shutdown signal");
}

int main(int argc, char **argv)
{
    /* through a pipe (the hosting site's console) every line has to go out at once */
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif
    bool force_cli = false;
    bool disable_cli = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--cli") == 0 || strcmp(argv[i], "--nogui") == 0)
            force_cli = true;
        else if (strcmp(argv[i], "--no-cli") == 0)
            disable_cli = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

#ifdef _WIN32
    TIMECAPS timer_caps;
    UINT timer_period = 0;
    if (timeGetDevCaps(&timer_caps, sizeof(timer_caps)) == TIMERR_NOERROR)
    {
        timer_period = timer_caps.wPeriodMin < 1 ? 1 : timer_caps.wPeriodMin;
        timeBeginPeriod(timer_period);
    }
#endif

    if (!disaster_init())
    {
#ifdef _WIN32
        if (timer_period)
            timeEndPeriod(timer_period);
#endif
        return 1;
    }

#ifdef _WIN32
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        Warn("Failed to set HIGH_PRIORITY_CLASS; continuing at normal priority.");
#endif

    if (!disaster_start())
    {
        Err("Failed to start server worker threads.");
#ifdef _WIN32
        if (timer_period)
            timeEndPeriod(timer_period);
#endif
        return 1;
    }

    bool use_cli = force_cli || (!disable_cli && admin_cli_stdin_is_tty());
    if (use_cli)
    {
        if (!admin_cli_spawn())
        {
            /* Could not get a thread; fall back to running it inline. A signal
               will not be able to interrupt the read in that case. */
            Warn("Could not start the admin CLI thread; running it on the main thread.");
            admin_cli_run();
            disaster_shutdown();
        }
    }
    else
    {
        Info("Admin CLI disabled because stdin is not an interactive terminal. Use --cli to force it.");
    }

    int result = disaster_wait();

#ifdef _WIN32
    if (timer_period)
        timeEndPeriod(timer_period);
#endif

    return result;
}
