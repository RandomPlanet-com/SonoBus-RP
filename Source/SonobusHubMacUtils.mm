// SonobusHubMacUtils.mm — own code, see SonobusHubMacUtils.h. Hub target only (CMakeLists.txt).
#include "SonobusHubMacUtils.h"

#if JUCE_MAC
#import <Foundation/Foundation.h>

void hubDisableAppNap() {
    // The token must stay alive for the whole process: ending the activity (or letting ARC release the token)
    // re-enables App Nap. Idempotent — a second call keeps the first token.
    static id sActivity = nil;
    if (sActivity != nil) return;
    NSProcessInfo* info = [NSProcessInfo processInfo];
    if (![info respondsToSelector:@selector(beginActivityWithOptions:reason:)]) return;
    sActivity = [info beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
                                        reason:@"SonoBus RP Hub real-time audio"];
#if !__has_feature(objc_arc)
    [sActivity retain];
#endif
}
#endif
