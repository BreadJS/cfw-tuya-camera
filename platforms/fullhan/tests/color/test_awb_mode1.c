#include "../../src/native/isp/isp_awb_mode1.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned rng=17;
static unsigned random_word(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void){
 const struct fh_awb_mode1_config cfg=FH_AWB_MODE1_DAY_CONFIG;
 struct fh_awb_state state={.gains={600,512,1300},.initialized=1};
 struct fh_awb_mode1_result out;struct fh_awb_sample s={.generation=1};
 for(unsigned i=0;i<9;i++)s.cell[i]=(struct fh_awb_cell){100000,100000,100000,100};
 assert(fh_awb_mode1_plan(&cfg,&state,&s,&out)==1);
 assert(out.pre_luma.red==512&&out.pre_luma.green==512&&out.pre_luma.blue==512);
 assert(out.gains.red==594&&out.gains.green==512&&out.gains.blue==1268);
 assert(out.selected_cell==4&&out.selected_weight==0&&out.accepted_cells==9);
 assert(fh_awb_mode1_plan(&cfg,&state,&s,&out)==-ESTALE);
 struct fh_awb_state before=state;
 s.generation=2;for(unsigned i=0;i<9;i++)s.cell[i]=(struct fh_awb_cell){100,100,100,100};
 assert(fh_awb_mode1_plan(&cfg,&state,&s,&out)==0&&state.gains.red==before.gains.red&&state.generation==2);
 s.generation=3;for(unsigned i=0;i<9;i++)s.cell[i]=(struct fh_awb_cell){409000,100000,100000,100};
 assert(fh_awb_mode1_plan(&cfg,&state,&s,&out)==0);
 for(unsigned n=0;n<10000;n++){
  s.generation++;
  for(unsigned i=0;i<9;i++){s.cell[i]=(struct fh_awb_cell){random_word(),random_word(),random_word(),random_word()};if(n%2){unsigned count=random_word()%10000+1;s.cell[i]=(struct fh_awb_cell){(random_word()%4080+1)*count,(random_word()%4080+1)*count,(random_word()%4080+1)*count,count};}}
  int rc=fh_awb_mode1_plan(&cfg,&state,&s,&out);assert(rc>=0);
  assert(state.gains.red>=256&&state.gains.red<=2048&&state.gains.green==512&&state.gains.blue>=256&&state.gains.blue<=2048);
 }
 puts("Mode1 guarded target + bounded smoothing:10000 mixed/adversarial samples PASS");
 return 0;
}
