#include "bindings.hpp"

#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace {
class BindingsTest : public testing::Test {
    void SetUp() override {
        RecordProperty("part", "2");
        RecordProperty("requirement", "spec-8.3");
    }
};

TEST_F(BindingsTest, ModifierOrderAndXkbAliasesNormalizeWithoutInferringShift) {
    auto a = seaside::parse_chord("Ctrl+Super+A");
    auto b = seaside::parse_chord("Super+Ctrl+A");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(*a, *b);
    auto prior = seaside::parse_chord("Prior");
    auto page_up = seaside::parse_chord("Page_Up");
    auto upper_d = seaside::parse_chord("D");
    auto lower_d = seaside::parse_chord("d");
    ASSERT_TRUE(prior);
    ASSERT_TRUE(page_up);
    ASSERT_TRUE(upper_d);
    ASSERT_TRUE(lower_d);
    EXPECT_EQ(prior.value(), page_up.value());
    EXPECT_NE(upper_d.value(), lower_d.value());
    EXPECT_EQ(upper_d.value().modifiers, 0);
    const auto space = seaside::parse_chord("Super+space");
    ASSERT_TRUE(space);
    EXPECT_EQ(seaside::parse_chord("Super+Space"), space);
    EXPECT_EQ(seaside::format_chord(*space), "Super+Space");
    EXPECT_TRUE(seaside::parse_chord("XF86AudioMute"));
    EXPECT_TRUE(seaside::parse_chord("Ctrl+Alt+Shift+Super+F12"));
}

TEST_F(BindingsTest, MissingUnknownAndDuplicateChordTokensAreRejected) {
    for (auto key : {"", "Super+", "Super+Super+A", "Ctrl+Ctrl+A", "Fn+F12", "Hyper+A", "Super++A",
                     "Super+NotARealXkbKey", "MouseLeftDrag", "Super+MouseMiddleDrag"}) {
        SCOPED_TRACE(key);
        EXPECT_FALSE(seaside::parse_chord(key));
    }
    EXPECT_FALSE(seaside::parse_chord(std::string("A\0garbage", 9)));
}

TEST_F(BindingsTest, EveryApprovedKeyboardActionParsesWithItsArguments) {
    auto chord = seaside::parse_chord("Super+A").value();
    for (auto action : {"toggle_window_tiling",
                        "focus left",
                        "focus right",
                        "focus up",
                        "focus down",
                        "cycle_focus_mru",
                        "move_window left",
                        "swap_window right",
                        "resize_window up",
                        "resize_window down 1",
                        "workspace 1",
                        "workspace 10",
                        "move_to_workspace 3",
                        "toggle_window_pin",
                        "toggle_window_maximize",
                        "toggle_window_fullscreen",
                        "close_window",
                        "night_light on",
                        "night_light off",
                        "night_light toggle",
                        "exit",
                        "exec foot"}) {
        SCOPED_TRACE(action);
        auto parsed = seaside::parse_action(action, chord);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(seaside::parse_action(seaside::format_action(*parsed), chord), parsed);
    }
    EXPECT_EQ(seaside::parse_action("resize_window left", chord).value().argument, 32);
    EXPECT_EQ(seaside::parse_action("workspace 10", chord).value().argument, 10);
    EXPECT_EQ(seaside::parse_action("night_light off", chord).value().night_light,
              seaside::NightLightAction::off);
}

TEST_F(BindingsTest, ArityDirectionsAndIntegerOverflowAreRejected) {
    auto chord = seaside::parse_chord("A").value();
    for (auto action : {"",
                        "exit now",
                        "not_an_action",
                        "focus",
                        "focus diagonal",
                        "focus left extra",
                        "cycle_focus_mru extra",
                        "move_window",
                        "resize_window",
                        "resize_window left 0",
                        "resize_window left -1",
                        "resize_window left 1.0",
                        "resize_window left 2147483648",
                        "resize_window left 2 extra",
                        "workspace",
                        "workspace 0",
                        "workspace 11",
                        "move_to_workspace 11",
                        "workspace -1",
                        "workspace 2147483648",
                        "workspace 999999999999999999999999999999",
                        "workspace 1 extra",
                        "transfer_workspace DP-1",
                        "transfer_workspace connector:",
                        "night_light auto",
                        "exec",
                        "exec    "}) {
        SCOPED_TRACE(action);
        EXPECT_FALSE(seaside::parse_action(action, chord));
    }
    EXPECT_FALSE(seaside::parse_action(std::string("exec foo\0bar", 12), chord));
}

TEST_F(BindingsTest, TypedActionResultsRetainTheirSpecificKindsAndArguments) {
    using K = seaside::ActionKind;
    const std::array definitions{
        std::pair{"toggle_window_tiling", K::toggle_window_tiling},
        std::pair{"focus left", K::focus},
        std::pair{"cycle_focus_mru", K::cycle_focus_mru},
        std::pair{"move_window right", K::move_window},
        std::pair{"resize_window down 99", K::resize_window},
        std::pair{"swap_window up", K::swap_window},
        std::pair{"workspace 7", K::workspace},
        std::pair{"move_to_workspace 8", K::move_to_workspace},
        std::pair{"toggle_window_pin", K::toggle_window_pin},
        std::pair{"toggle_window_maximize", K::toggle_window_maximize},
        std::pair{"toggle_window_fullscreen", K::toggle_window_fullscreen},
        std::pair{"close_window", K::close_window},
        std::pair{"night_light toggle", K::night_light},
        std::pair{"exit", K::exit},
        std::pair{"exec foot", K::exec},
    };
    auto chord = seaside::parse_chord("A").value();
    for (auto [text, kind] : definitions) {
        SCOPED_TRACE(text);
        auto parsed = seaside::parse_action(text, chord);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->kind, kind);
    }
    for (auto [text, direction] :
         {std::pair{"left", seaside::Direction::left},
          std::pair{"right", seaside::Direction::right}, std::pair{"up", seaside::Direction::up},
          std::pair{"down", seaside::Direction::down}}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(seaside::parse_action(std::string("focus ") + text, chord).value().direction,
                  direction);
    }
    EXPECT_EQ(seaside::parse_action("move_window right", chord).value().direction,
              seaside::Direction::right);
    EXPECT_EQ(seaside::parse_action("swap_window up", chord).value().direction,
              seaside::Direction::up);
    auto resize = seaside::parse_action("resize_window down 99", chord);
    ASSERT_TRUE(resize);
    EXPECT_EQ(resize->direction, seaside::Direction::down);
    EXPECT_EQ(resize->argument, 99);
    EXPECT_EQ(seaside::parse_action("workspace 7", chord).value().argument, 7);
    EXPECT_EQ(seaside::parse_action("move_to_workspace 8", chord).value().argument, 8);
    EXPECT_EQ(seaside::parse_action("night_light on", chord).value().night_light,
              seaside::NightLightAction::on);
    EXPECT_EQ(seaside::parse_action("night_light off", chord).value().night_light,
              seaside::NightLightAction::off);
    EXPECT_EQ(seaside::parse_action("night_light toggle", chord).value().night_light,
              seaside::NightLightAction::toggle);
    EXPECT_EQ(seaside::parse_action("exec foot", chord).value().text, "foot");
    EXPECT_EQ(
        seaside::parse_action("move_window", seaside::parse_chord("Super+MouseLeftDrag").value())
            .value()
            .kind,
        K::move_window);
    EXPECT_EQ(
        seaside::parse_action("resize_window", seaside::parse_chord("Super+MouseRightDrag").value())
            .value()
            .kind,
        K::resize_window);
}

TEST_F(BindingsTest, MouseGestureAndActionContextsMustMatch) {
    auto left = seaside::parse_chord("Super+MouseLeftDrag").value();
    auto right = seaside::parse_chord("Super+MouseRightDrag").value();
    EXPECT_EQ(seaside::format_chord(left), "Super+MouseLeftDrag");
    EXPECT_EQ(seaside::format_chord(right), "Super+MouseRightDrag");
    const auto alias = seaside::parse_chord("Ctrl+Super+Page_Up");
    ASSERT_TRUE(alias);
    const auto canonical = seaside::format_chord(*alias);
    EXPECT_TRUE(canonical.starts_with("Super+Ctrl+"));
    EXPECT_EQ(seaside::parse_chord(canonical), alias);
    EXPECT_EQ(seaside::format_action(seaside::parse_action("move_window", left).value()),
              "move_window");
    EXPECT_EQ(seaside::format_action(seaside::parse_action("resize_window", right).value()),
              "resize_window");
    EXPECT_TRUE(seaside::parse_action("move_window", left));
    EXPECT_TRUE(seaside::parse_action("resize_window", right));
    EXPECT_FALSE(seaside::parse_action("resize_window", left));
    EXPECT_FALSE(seaside::parse_action("move_window", right));
    for (auto value :
         {"exit", "focus left", "exec foot", "move_window left", "resize_window right 32"}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(seaside::parse_action(value, left));
        EXPECT_FALSE(seaside::parse_action(value, right));
    }
}

TEST_F(BindingsTest, ShellCommandBytesArePreservedAsData) {
    auto chord = seaside::parse_chord("A").value();
    std::string command = "printf '%s\\n' \"$(echo untouched)\"; # comment\n echo next  ";
    auto parsed = seaside::parse_action("exec " + command, chord);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->text, command);
    EXPECT_EQ(parsed->kind, seaside::ActionKind::exec);
    EXPECT_EQ(seaside::format_action(*parsed), "exec " + command);
}

TEST_F(BindingsTest, SelectorsUseExactPrefixesAndOpaqueValues) {
    auto identity = seaside::parse_output_selector("identity:屏幕:serial");
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->value, "屏幕:serial");
    EXPECT_EQ(identity->kind, seaside::SelectorKind::identity);
    EXPECT_TRUE(seaside::parse_output_selector("connector:DP-1"));
    for (auto value : {"auto", "DP-1", "identity:", "connector:", "Connector:DP-1", "regex:.*",
                       "connector:DP-1\n"}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(seaside::parse_output_selector(value));
    }
}

TEST_F(BindingsTest, RetiredWorkspaceTransferIsRejected) {
    const auto chord = seaside::parse_chord("A").value();
    EXPECT_FALSE(seaside::parse_action("transfer_workspace identity:Model A", chord));
}

TEST_F(BindingsTest, DefaultsHaveOnlyTheApprovedBindings) {
    auto map = seaside::default_bindings();
    ASSERT_EQ(map.size(), 44U);
    EXPECT_EQ(map.at(seaside::parse_chord("Super+Return").value()).text, "foot");
    const auto &launcher = map.at(seaside::parse_chord("Super+Space").value());
    EXPECT_EQ(launcher.kind, seaside::ActionKind::exec);
    EXPECT_EQ(launcher.text, "wofi --show drun");
    const auto &region = map.at(seaside::parse_chord("Super+S").value());
    EXPECT_EQ(region.kind, seaside::ActionKind::exec);
    EXPECT_EQ(region.text, "seaside-screenshot region");
    const auto &screen = map.at(seaside::parse_chord("Super+Shift+S").value());
    EXPECT_EQ(screen.kind, seaside::ActionKind::exec);
    EXPECT_EQ(screen.text, "seaside-screenshot screen");
    EXPECT_EQ(map.at(seaside::parse_chord("XF86AudioRaiseVolume").value()).text,
              "wpctl set-volume --limit 1 @DEFAULT_AUDIO_SINK@ 5%+");
    EXPECT_EQ(map.at(seaside::parse_chord("Super+F").value()).kind,
              seaside::ActionKind::toggle_window_maximize);
    EXPECT_EQ(map.at(seaside::parse_chord("Super+Shift+F").value()).kind,
              seaside::ActionKind::toggle_window_fullscreen);
    EXPECT_EQ(map.at(seaside::parse_chord("Super+Q").value()).kind,
              seaside::ActionKind::close_window);
    EXPECT_EQ(map.at(seaside::parse_chord("Super+P").value()).kind,
              seaside::ActionKind::toggle_window_pin);
    EXPECT_EQ(map.at(seaside::parse_chord("Super+Shift+E").value()).kind,
              seaside::ActionKind::exit);
    for (const auto &[key, direction] : {std::pair{"Left", seaside::Direction::left},
                                         {"Right", seaside::Direction::right},
                                         {"Up", seaside::Direction::up},
                                         {"Down", seaside::Direction::down}}) {
        const auto chord = seaside::parse_chord(std::string("Super+Shift+") + key).value();
        const auto &move = map.at(chord);
        EXPECT_EQ(move.kind, seaside::ActionKind::move_window);
        EXPECT_EQ(move.direction, direction);
    }
    for (int workspace = 1; workspace <= 10; ++workspace) {
        const auto digit = std::to_string(workspace % 10);
        const auto &select = map.at(seaside::parse_chord("Super+" + digit).value());
        EXPECT_EQ(select.kind, seaside::ActionKind::workspace);
        EXPECT_EQ(select.argument, workspace);
        const auto &move = map.at(seaside::parse_chord("Super+Shift+" + digit).value());
        EXPECT_EQ(move.kind, seaside::ActionKind::move_to_workspace);
        EXPECT_EQ(move.argument, workspace);
    }
    for (auto key : {"Super+B", "Super+N", "Super+M"})
        EXPECT_FALSE(map.contains(seaside::parse_chord(key).value()));
}
} // namespace
