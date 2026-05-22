#include "act.h"

void random_act() {
  int random_num = random(100);
  int x = random(-25, 25);

  if (random_num > 80) {
    head_move(x, random(-20, 10), 10);
    head_center();
  } else if (random_num > 50) {
    head_nod(2);
  }
}
