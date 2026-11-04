#include "isp_awb.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static struct fh_awb_config policy={16,4000,128,2048,128,2048,3};
static struct fh_awb_sample sample;
static void cell(unsigned at,unsigned red,unsigned green,unsigned blue,unsigned count)
{sample.cell[at]=(struct fh_awb_cell){red*count,green*count,blue*count,count};}
static void fill(unsigned red,unsigned green,unsigned blue)
{memset(&sample,0,sizeof sample);for(unsigned i=0;i<9;i++)cell(i,red,green,blue,100);}
static struct fh_awb_state initial(void)
{struct fh_awb_state state;const struct fh_awb_gains unity={512,512,512};assert(!fh_awb_init(&state,&unity));return state;}
static int plan(struct fh_awb_state *state,struct fh_awb_result *result)
{sample.generation=state->generation+1;return fh_awb_plan(&policy,state,&sample,result);}
static unsigned difference(unsigned a,unsigned b){return a>b?a-b:b-a;}
static void convergence(unsigned red,unsigned green,unsigned blue,unsigned expected_red,unsigned expected_blue)
{
    struct fh_awb_state state=initial();struct fh_awb_result result;fill(red,green,blue);
    for(unsigned i=0;i<160;i++) {
        struct fh_awb_gains before=state.gains;
        assert(plan(&state,&result)==1);assert(result.accepted_cells==9);
        assert(result.gains.green==512&&result.gains.red>=256&&result.gains.red<=2048&&result.gains.blue>=256&&result.gains.blue<=2048);
        assert(difference(before.red,result.gains.red)<=32&&difference(before.blue,result.gains.blue)<=32);
    }
    assert(difference(state.gains.red,expected_red)<=2&&difference(state.gains.blue,expected_blue)<=2);
}
int main(void)
{
    struct fh_awb_state state=initial();struct fh_awb_result result;
    fill(512,512,512);assert(plan(&state,&result)==1&&!result.changed&&result.accepted_cells==9);
    convergence(960,640,320,341,1024); /* warm neutral reference remains usable */
    convergence(320,640,960,1024,341); /* cooler illumination */
    convergence(1600,800,300,256,1365);
    /* A single monitor has vastly more counted/bright pixels than the room,
     * but lies outside the spatial ratio consensus even inside broad gates. */
    state=initial();fill(512,512,512);cell(0,300,500,1500,2000000);
    assert(plan(&state,&result)==1&&result.accepted_cells==8&&!result.changed);
    /* Ninth spatial cell is included, avoiding the historic eight-cell trap. */
    state=initial();fill(100,800,100);cell(0,700,700,700,100);cell(1,700,700,700,100);cell(8,700,700,700,100);
    assert(plan(&state,&result)==1&&result.accepted_cells==3&&!result.changed);
    /* Colored scenes outside the calibrated envelope do not drive gray-world
     * normalization. In-envelope uniform scenes remain an explicit ambiguity. */
    state=initial();fill(100,800,100);assert(!plan(&state,&result)&&!result.changed);
    fill(100,200,1400);assert(!plan(&state,&result));
    fill(1400,200,100);assert(!plan(&state,&result));
    fill(0,0,0);assert(!plan(&state,&result));
    fill(4000,4000,4000);assert(!plan(&state,&result));
    fill(512,512,512);sample.cell[0].count=0;sample.cell[1].green=0;sample.cell[2].red=400000;
    assert(plan(&state,&result)==1&&result.accepted_cells==6);
    /* No sufficiently large consensus: retain gains instead of chasing it. */
    fill(500,500,500);for(unsigned i=0;i<3;i++)cell(i,300,512,800,100);for(unsigned i=3;i<6;i++)cell(i,800,512,300,100);
    policy.minimum_cells=4;assert(!plan(&state,&result));policy.minimum_cells=3;
    struct fh_awb_state before=state;struct fh_awb_result untouched={0};
    result=untouched;assert(fh_awb_plan(&policy,&state,&sample,&result)==-ESTALE);
    assert(!memcmp(&state,&before,sizeof state)&&!memcmp(&result,&untouched,sizeof result));
    sample.generation=0;assert(fh_awb_plan(&policy,&state,&sample,&result)==-EINVAL);
    policy.minimum_cells=2;sample.generation=before.generation+1;assert(fh_awb_plan(&policy,&state,&sample,&result)==-EINVAL);policy.minimum_cells=3;
    assert(!memcmp(&state,&before,sizeof state));
    /* Planning on a copy permits coherent owner publication/commit. */
    struct fh_awb_state candidate=state;fill(960,640,320);assert(plan(&candidate,&result)==1);assert(!memcmp(&state,&before,sizeof state));
    const struct fh_awb_gains scaled={512,1024,2048};assert(!fh_awb_init(&state,&scaled));assert(state.gains.red==256&&state.gains.green==512&&state.gains.blue==1024);
    for(unsigned i=0;i<9;i++)sample.cell[i]=(struct fh_awb_cell){UINT32_MAX,1,UINT32_MAX,1};
    assert(!plan(&state,&result));
    assert(fh_awb_init(NULL,&scaled)==-EINVAL);
    puts("PASS native AWB: nine-cell consensus, warm/cool convergence, dominant monitor rejection, colored/dark/clipped holds, gain/slew bounds, stale generation and transactional state");return 0;
}
