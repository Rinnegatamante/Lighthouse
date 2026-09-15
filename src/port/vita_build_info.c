#include "build.h"

char gBuildVersion[] = "0.1.0-vita-trophies";
u16 gBuildVersionMajor = 0;
u16 gBuildVersionMinor = 1;
u16 gBuildVersionPatch = 0;
char gGitBranch[] = "ps-vita-trophy-support";
char gGitCommitHash[] = "68079d04";
char gGitCommitTag[] = "vita-trophies";
char gBuildTeam[] = "Lighthouse";
char gBuildDate[] = __DATE__;
char gBuildMakeOption[] = "Makefile.vita";

// Compatibility with the locally installed vitaGL diagnostic build.
void mdkr_vita_boot_log(const char* message) {
    (void)message;
}
