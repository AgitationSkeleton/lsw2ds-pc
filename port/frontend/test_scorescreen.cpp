#include "scorescreen.h"
#include <cstdio>
namespace { int checks=0, fails=0;
void check(bool ok,const char* what){++checks;std::printf("  %s %s\n",ok?"ok  ":"FAIL",what);if(!ok)++fails;} }
int main(){
    std::printf("score screen timer\n");
    check(kLsw2ScoreSeconds==15,"the specified fifteen seconds");
    check(!lsw2_score_should_press(false,false,1000,900),"never outside the score state");
    check(!lsw2_score_should_press(true,false,100,100),"not on arrival");
    check(!lsw2_score_should_press(true,false,114,100),"not at fourteen");
    check(lsw2_score_should_press(true,false,115,100),"fires at fifteen");
    check(lsw2_score_should_press(true,false,900,100),"and stays fired if a frame is missed");
    check(!lsw2_score_should_press(true,true,900,100),"but ONCE - a second press would commit the next screen's row");
    /* The counter is a free-running word. A visit that straddles the wrap must
     * still expire; comparing now >= entered+15 would hang it forever. */
    check(lsw2_score_should_press(true,false,10u,0xFFFFFFF0u),"survives the counter wrapping");
    std::printf("\n%d checks, %d failed\n",checks,fails);
    if(fails) return 1;
    std::printf("all checks passed\n");
    return 0;
}
