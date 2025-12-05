#include <iostream>
#include <algorithm>
#include "core/Engine.h"  
#include "exercises/ExerciseRegistry.h"

int main(int argc, char* argv[]) {
    auto& registry = ExerciseRegistry::instance();
    auto exercises = registry.getExercises();
    
    // Sort exercises by NUMBER (1, 2, 3... 10)
    std::sort(exercises.begin(), exercises.end(), 
              [](const Exercise& a, const Exercise& b) {
                  return a.number < b.number;
              });
    
    int choice = -1;
    
    if (argc > 1) {
        choice = std::atoi(argv[1]);
    } else {
        std::cout << "\n=== AceEngine - OpenGL Exercises ===" << std::endl;
        for (size_t i = 0; i < exercises.size(); i++) {
            std::cout << (i + 1) << ". " << exercises[i].name << std::endl;
        }
        std::cout << "0. Exit" << std::endl;
        std::cout << "\nEnter your choice: ";
        std::cin >> choice;
    }
    
    if (choice == 0) {
        std::cout << "Goodbye!" << std::endl;
        return 0;
    }
    
    if (choice < 1 || choice > (int)exercises.size()) {
        std::cerr << "Invalid choice!" << std::endl;
        return 1;
    }
    
    Engine engine;
    if (!engine.init(640, 480, false)) {
        return 1;
    }
    
    exercises[choice - 1].run(engine.getWindow());
    
    return 0;
}