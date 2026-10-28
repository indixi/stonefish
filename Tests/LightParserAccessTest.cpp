// Compile-only regression: ROSScenarioParser must be allowed to reuse ParseColor.
#include "core/ScenarioParser.h"
#include "actuators/Light.h"
#include <type_traits>

struct DerivedLightParser : sf::ScenarioParser
{
    using sf::ScenarioParser::ParseColor;
};
static_assert(std::is_same_v<decltype(&sf::Light::setLightProperties),
              void (sf::Light::*)(const sf::Color&, sf::Scalar, bool)>);
