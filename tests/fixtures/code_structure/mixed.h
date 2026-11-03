struct Context { int typing = 0; int scroll = 0; };
extern Context first, second;
Context& GetContext(int key);
int& Typing();
void Persist(int value);
void Paint(int value);
void Edit(int value);
