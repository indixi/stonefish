// CPU-only checks. No SDL window or OpenGL context is created.
#include "graphics/OpenGLLight.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#define CHECK(condition) do { if(!(condition)) { \
    std::cerr << "Check failed at line " << __LINE__ << ": " #condition "\n"; \
    return 1; } } while(false)

class TestLight final : public sf::OpenGLLight
{
public:
    explicit TestLight(sf::LightType type)
        : OpenGLLight(glm::vec3(0.f), .012f, glm::vec3(0.f), 0.f), type(type)
    { colorLi = glm::vec4(0.f); }
    sf::LightType getType() const override { return type; }
    void SetupShader(sf::LightUBO*) override {}
private:
    sf::LightType type;
};

int main()
{
    TestLight point(sf::LightType::POINT);
    point.setColorIntensity(glm::vec3(1.f, .25f, 0.f), 25.f);
    CHECK(point.getColorLi() == glm::vec4(1.f, .25f, 0.f, 25.f));
    point.setColorIntensity(glm::vec3(0.f, 0.f, 1.f), 0.f);
    CHECK(point.getColorLi().w == 0.f);
    point.setColorIntensity(glm::vec3(0.f, 1.f, 0.f), 50.f);
    CHECK(point.getColorLi() == glm::vec4(0.f, 1.f, 0.f, 50.f));
    point.setColorIntensity(glm::vec3(0.f, 1.f, 0.f), 50.f);
    CHECK(point.getColorLi().w == 50.f); // Absolute, not cumulative.
    point.setColorIntensity(glm::vec3(-1.f, .5f, 2.f), -1.f);
    CHECK(point.getColorLi() == glm::vec4(0.f, .5f, 1.f, 0.f));
    const glm::vec4 previous = point.getColorLi();
    point.setColorIntensity(glm::vec3(std::numeric_limits<float>::quiet_NaN()), 1.f);
    CHECK(point.getColorLi() == previous);
    point.setColorIntensity(glm::vec3(1.f), std::numeric_limits<float>::infinity());
    CHECK(point.getColorLi() == previous);
    point.SwitchOff();
    point.setColorIntensity(glm::vec3(1.f), 100.f);
    CHECK(!point.isActive()); // Appearance updates do not undo explicit off.

    TestLight p0(sf::LightType::POINT), p1(sf::LightType::POINT),
              s0(sf::LightType::SPOT), s1(sf::LightType::SPOT);
    std::vector<sf::OpenGLLight*> lights{&p0, &s0, &p1, &s1};
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 0) == 0);
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 1) == 0);
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 2) == 1);
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 3) == 1);
    p0.SwitchOff();
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 0) == -1);
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 2) == 0);
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 3) == 1);
    s0.SwitchOff();
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 3) == 0);
    p0.SwitchOn();
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 2) == 1);
    CHECK(sf::OpenGLLight::getActiveIndex(lights, 99) == -1);
    std::cout << "Engine light appearance and active-index checks passed\n";
}
