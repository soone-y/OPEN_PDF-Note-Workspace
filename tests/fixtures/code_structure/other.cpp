#include "mixed.h"
static int private_state = 0;
void UpdatePrivate() { private_state++; }
void Parameter(Context& receiver) { receiver.scroll++; }
