#ifndef EXERCISE_REGISTRY_H
#define EXERCISE_REGISTRY_H

#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <functional>

struct Exercise {
    int number;           // Exercise number (1-10)
    std::string name;
    std::function<void(GLFWwindow*)> run;
};

class ExerciseRegistry {
public:
    static ExerciseRegistry& instance() {
        static ExerciseRegistry registry;
        return registry;
    }
    
    void registerExercise(int number, const std::string& name, std::function<void(GLFWwindow*)> func) {
        exercises.push_back({number, name, func});
    }
    
    const std::vector<Exercise>& getExercises() const {
        return exercises;
    }
    
private:
    ExerciseRegistry() = default;
    std::vector<Exercise> exercises;
};

// Helper macro to auto-register exercises with number
#define REGISTER_EXERCISE(num, name, func) \
    static bool registered_##func = []() { \
        ExerciseRegistry::instance().registerExercise(num, name, func); \
        return true; \
    }();

#endif