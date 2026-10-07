#include "diagnostics/foreground_policy.h"
#include <iostream>
#include <vector>
using relink::diagnostics::ForegroundPolicy;
int main() {
    int count = 0, failed = 0;
    auto check = [&](bool ok, const char* text) { ++count; failed += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; };
    using Handle = ForegroundPolicy::Handle;
    Handle foreground = 2;
    std::vector<Handle> switches;
    auto current = [&]() { return foreground; };
    auto game = [&]() { switches.push_back(1); foreground=1; return true; };
    auto ide = [&]() { switches.push_back(2); foreground=2; return true; };
    {
        ForegroundPolicy once(false, 1, 2, current, game, ide);
        check(once.begin() && foreground==1, "standalone_activates_once");
        check(foreground==1 && switches.size()==1, "capture_and_ocr_have_no_intermediate_restore");
        check(once.finish() && foreground==2, "standalone_restores_at_end");
        check(once.finish() && once.restoreRequests==1, "finish_is_idempotent");
        check(!once.begin(), "completed_session_does_not_restart");
    }
    check(switches==std::vector<Handle>{1,2}, "destructor_does_not_restore_twice");
    switches.clear(); game();
    for (int i=0;i<5;++i) {
        ForegroundPolicy nested(true,1,2,current,game,ide);
        check(nested.begin() && foreground==1, "batch_child_requires_existing_game_foreground");
        check(nested.finish() && nested.activationRequests==0 && nested.restoreRequests==0, "batch_child_never_switches_foreground");
    }
    check(switches==std::vector<Handle>{1}, "five_probes_keep_game_front_continuously");
    ide();check(switches==std::vector<Handle>{1,2}, "outer_batch_restores_only_once");
    switches.clear();
    {
        ForegroundPolicy missing(true,1,2,current,game,ide);
        check(!missing.begin() && !missing.finish(), "caller_owned_mismatch_does_not_steal_focus");
    }
    check(switches.empty(), "failed_child_has_no_hidden_destructor_restore");
    foreground=1;
    {
        ForegroundPolicy interrupted(true,1,2,current,game,ide);
        check(interrupted.begin(), "child_started_with_valid_owner");
        foreground=3;
        check(!interrupted.finish(), "external_focus_loss_ends_batch_child");
    }
    check(switches.empty() && foreground==3, "child_does_not_reacquire_after_user_switch");
    foreground=2;
    { ForegroundPolicy exceptionPath(false,1,2,current,game,ide); check(exceptionPath.begin(), "exception_path_started"); }
    check(foreground==2 && switches==std::vector<Handle>{1,2}, "scope_exit_restores_once");
    switches.clear(); foreground=1;
    {
        ForegroundPolicy failure(false,1,2,current,game,[](){return false;});
        check(failure.begin() && failure.activationRequests==0, "already_game_does_not_activate_again");
        check(!failure.finish() && !failure.finish() && failure.restoreRequests==1, "restore_failure_not_retried_in_loop");
    }
    std::cout << "FOREGROUND_POLICY_TESTS=" << (failed?"FAIL":"PASS") << "; assertions=" << count << "; failures=" << failed << "; real_focus_switches=0\n";
    return failed?1:0;
}
