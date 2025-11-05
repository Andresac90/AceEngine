#include "core/Engine.h"
#include "exercises/exercise1.h"

int main() {
    Engine engine;
    
    // Initialize engine (windowed mode)
    if (!engine.init(640, 480, false)) {
        return 1;
    }
    
    // For fullscreen, use: engine.init(0, 0, true);
    
    // Run exercise
    runExercise1(engine.getWindow());
    
    // Engine automatically shuts down via destructor
    return 0;
}