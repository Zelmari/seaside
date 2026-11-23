#include "config.hpp"

#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <random>

#ifndef SEASIDE_SOURCE_ROOT
#define SEASIDE_SOURCE_ROOT "."
#endif

namespace {
class ConfigTest : public testing::Test {
    void SetUp() override {
        RecordProperty("part", "2");
        RecordProperty("requirement", "spec-8.1-8.2-9");
    }
};

TEST_F(ConfigTest, CompiledDefaultsMatchApprovedValues) {
    auto value = seaside::default_config();
    EXPECT_TRUE(value.autostart.commands.empty());
    EXPECT_EQ(value.wallpaper.path, "/usr/share/seaside/wallpapers/black.png");
    EXPECT_TRUE(value.window.tiling);
    EXPECT_EQ(value.window.layout, seaside::Layout::dwindle);
    EXPECT_EQ(value.window.gaps_outer, 10);
    EXPECT_EQ(value.window.gaps_inner, 5);
    EXPECT_EQ(value.window.border_width, 2);
    EXPECT_TRUE(value.window.titlebars);
    EXPECT_EQ(value.workspaces.mode, seaside::WorkspaceMode::global);
    EXPECT_FALSE(value.monitors.primary);
    EXPECT_FALSE(value.monitors.defaults.resolution);
    EXPECT_FALSE(value.monitors.defaults.refresh_rate);
    EXPECT_FALSE(value.monitors.defaults.position);
    EXPECT_EQ(value.monitors.defaults.scale, 1.0);
    EXPECT_EQ(value.monitors.defaults.rotation, 0);
    EXPECT_TRUE(value.monitors.output.empty());
    EXPECT_EQ(value.dwindle.insertion, seaside::Insertion::focused);
    EXPECT_EQ(value.dwindle.split_ratio, 0.5);
    EXPECT_EQ(value.scrolling.default_column_width, 0.5);
    EXPECT_EQ(value.input.mouse.profile, "flat");
    EXPECT_EQ(value.input.mouse.sensitivity, 0.0);
    EXPECT_EQ(value.idle.display_off_after_minutes, 15);
    EXPECT_TRUE(value.render.direct_scanout);
    EXPECT_EQ(value.render.adaptive_sync, seaside::AdaptiveSync::off);
    EXPECT_TRUE(value.render.allow_tearing);
    EXPECT_EQ(value.render.fullscreen_aspect, seaside::FullscreenAspect::preserve);
    EXPECT_FALSE(value.night_light.enabled);
    EXPECT_EQ(value.night_light.temperature_kelvin, 4500);
    EXPECT_EQ(value.keybinds.size(), 38U);
}

TEST_F(ConfigTest, CanonicalGenerationMatchesIndependentReferenceAndCompiledDefaults) {
    std::ifstream file(std::string(SEASIDE_SOURCE_ROOT) +
                       "/Specification/SEASIDE_CONFIG_EXAMPLE.toml");
    ASSERT_TRUE(file);
    std::string reference{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(seaside::default_template(), reference);
    auto parsed = seaside::parse_config(reference);
    ASSERT_TRUE(parsed) << (parsed ? "" : parsed.error().message);
    EXPECT_EQ(*parsed, seaside::default_config());
    auto empty = seaside::parse_config("");
    ASSERT_TRUE(empty);
    EXPECT_EQ(*empty, seaside::default_config());
}

TEST_F(ConfigTest, ApprovedFieldsAcceptValidValues) {
    const std::array valid{
        "[autostart]\ncommands = ['foot']",
        "[wallpaper]\npath = '/tmp/壁紙.webp'",
        "[window]\ntiling=false\nlayout='scrolling'\ngaps_outer=0\ngaps_inner=512\nborder_width=64",
        "[workspaces]\nmode='per-monitor'",
        "[monitors]\nprimary='connector:DP-1'\nresolution='1920x1080'\nrefresh_rate=144\nscale=1."
        "25\nrotation=270\nposition=[-1920,0]",
        "[monitors]\nprimary='identity:serial'\nresolution='auto'\nrefresh_rate='auto'\nposition='"
        "auto'",
        "[dwindle]\ninsertion='largest'\nsplit_ratio=0.95",
        "[scrolling]\ndefault_column_width=4",
        "[input.mouse]\nprofile='flat'\nsensitivity=-1",
        "[idle]\ndisplay_off_after_minutes=0",
        "[render]\ndirect_scanout=false\nadaptive_sync='always'\nallow_tearing=true\nfullscreen_"
        "aspect='stretch'",
        "[render]\nadaptive_sync='fullscreen'",
        "[night_light]\nenabled=true\ntemperature_kelvin=1000",
    };
    for (auto text : valid) {
        SCOPED_TRACE(text);
        auto result = seaside::parse_config(text);
        EXPECT_TRUE(result) << (result ? "" : result.error().field + ": " + result.error().message);
    }
}

TEST_F(ConfigTest, NondefaultValuesPopulateEveryTypedConfigurationField) {
    auto parsed = seaside::parse_config(R"(
[autostart]
commands=['foot','echo hello']
[wallpaper]
path='/tmp/壁紙.webp'
[window]
tiling=false
titlebars=false
layout='scrolling'
gaps_outer=21
gaps_inner=22
border_width=23
active_border_color='#00d0Ef'
inactive_border_color='#123456'
[appearance]
color_scheme='light'
[audio]
default_output='alsa_output.usb-Example'
default_input='alsa_input.usb-Example'
[power]
profile='performance'
[workspaces]
mode='per-monitor'
[monitors]
primary='identity:serial'
resolution='2560x1440'
refresh_rate=59.94
scale=1.25
rotation=270
position=[-2560,100]
[dwindle]
insertion='largest'
split_ratio=0.25
[scrolling]
default_column_width=0.75
[input.mouse]
profile='flat'
sensitivity=-0.7
scroll_direction='inverse'
[idle]
display_off_after_minutes=42
[render]
direct_scanout=false
adaptive_sync='always'
allow_tearing=false
fullscreen_aspect='stretch'
[render.blur]
enabled=true
strength=0.75
passes=4
[night_light]
enabled=true
temperature_kelvin=6000
[keybinds]
'Super+Q'='close_window'
)");
    ASSERT_TRUE(parsed);
    const auto &c = *parsed;
    EXPECT_EQ(c.autostart.commands, (std::vector<std::string>{"foot", "echo hello"}));
    EXPECT_EQ(c.wallpaper.path, "/tmp/壁紙.webp");
    EXPECT_FALSE(c.window.tiling);
    EXPECT_FALSE(c.window.titlebars);
    EXPECT_EQ(c.window.layout, seaside::Layout::scrolling);
    EXPECT_EQ(c.window.gaps_outer, 21);
    EXPECT_EQ(c.window.gaps_inner, 22);
    EXPECT_EQ(c.window.border_width, 23);
    EXPECT_EQ(c.window.active_border_color, 0x00d0efU);
    EXPECT_EQ(c.window.inactive_border_color, 0x123456U);
    EXPECT_EQ(c.appearance.color_scheme, seaside::ColorScheme::light);
    EXPECT_EQ(c.audio.default_output, "alsa_output.usb-Example");
    EXPECT_EQ(c.audio.default_input, "alsa_input.usb-Example");
    EXPECT_EQ(c.power.profile, seaside::PowerProfile::performance);
    EXPECT_EQ(c.workspaces.mode, seaside::WorkspaceMode::per_monitor);
    ASSERT_TRUE(c.monitors.primary);
    EXPECT_EQ(c.monitors.primary->kind, seaside::SelectorKind::identity);
    EXPECT_EQ(c.monitors.primary->value, "serial");
    EXPECT_EQ(c.monitors.defaults.resolution, (seaside::Resolution{2560, 1440}));
    EXPECT_EQ(c.monitors.defaults.refresh_rate, 59.94);
    EXPECT_EQ(c.monitors.defaults.scale, 1.25);
    EXPECT_EQ(c.monitors.defaults.rotation, 270);
    EXPECT_EQ(c.monitors.defaults.position, (seaside::Position{-2560, 100}));
    EXPECT_EQ(c.dwindle.insertion, seaside::Insertion::largest);
    EXPECT_EQ(c.dwindle.split_ratio, 0.25);
    EXPECT_EQ(c.scrolling.default_column_width, 0.75);
    EXPECT_EQ(c.input.mouse.profile, "flat");
    EXPECT_EQ(c.input.mouse.sensitivity, -0.7);
    EXPECT_EQ(c.input.mouse.scroll_direction, seaside::ScrollDirection::inverse);
    EXPECT_EQ(c.idle.display_off_after_minutes, 42);
    EXPECT_FALSE(c.render.direct_scanout);
    EXPECT_EQ(c.render.adaptive_sync, seaside::AdaptiveSync::always);
    EXPECT_FALSE(c.render.allow_tearing);
    EXPECT_EQ(c.render.fullscreen_aspect, seaside::FullscreenAspect::stretch);
    EXPECT_TRUE(c.render.blur.enabled);
    EXPECT_EQ(c.render.blur.strength, 0.75);
    EXPECT_EQ(c.render.blur.passes, 4);
    EXPECT_TRUE(c.night_light.enabled);
    EXPECT_EQ(c.night_light.temperature_kelvin, 6000);
    ASSERT_EQ(c.keybinds.size(), 1U);
    EXPECT_EQ(c.keybinds.begin()->second.kind, seaside::ActionKind::close_window);
    EXPECT_EQ(c.keybinds.begin()->first, seaside::parse_chord("Super+Q").value());
    EXPECT_EQ(
        seaside::parse_config("[render]\nadaptive_sync='fullscreen'").value().render.adaptive_sync,
        seaside::AdaptiveSync::fullscreen);
}

TEST_F(ConfigTest, EveryTableRejectsUnknownFields) {
    for (auto table : {"autostart", "wallpaper", "window", "appearance", "audio", "power",
                       "workspaces", "monitors", "dwindle", "scrolling", "input", "input.mouse",
                       "idle", "render", "render.blur", "night_light"}) {
        const std::string text = "[" + std::string(table) + "]\nunknown_setting=1";
        SCOPED_TRACE(text);
        auto parsed = seaside::parse_config(text);
        ASSERT_FALSE(parsed);
        EXPECT_EQ(parsed.error().field, std::string(table) + ".unknown_setting");
    }
}

TEST_F(ConfigTest, NumericEndpointsAndAdjacentValuesAreChecked) {
    struct Range {
        const char *table;
        const char *key;
        const char *minimum;
        const char *maximum;
        const char *below;
        const char *above;
    };
    const std::array ranges{
        Range{"window", "gaps_outer", "0", "512", "-1", "513"},
        Range{"window", "gaps_inner", "0", "512", "-1", "513"},
        Range{"window", "border_width", "0", "64", "-1", "65"},
        Range{"idle", "display_off_after_minutes", "0", "1440", "-1", "1441"},
        Range{"night_light", "temperature_kelvin", "1000", "10000", "999", "10001"},
        Range{"monitors", "scale", "0.5", "4.0", "0.49999", "4.00001"},
        Range{"dwindle", "split_ratio", "0.05", "0.95", "0.04999", "0.95001"},
        Range{"scrolling", "default_column_width", "0.10", "4.0", "0.09999", "4.00001"},
        Range{"input.mouse", "sensitivity", "-1.0", "1.0", "-1.00001", "1.00001"},
    };
    for (const auto &range : ranges) {
        for (auto number : {range.minimum, range.maximum, range.below, range.above}) {
            std::string text = "[" + std::string(range.table) + "]\n" + range.key + "=" + number;
            SCOPED_TRACE(text);
            EXPECT_EQ(seaside::parse_config(text).has_value(),
                      number == range.minimum || number == range.maximum);
        }
    }
    for (auto rotation : {0, 90, 180, 270}) {
        SCOPED_TRACE(rotation);
        EXPECT_TRUE(seaside::parse_config("[monitors]\nrotation=" + std::to_string(rotation)));
    }
    for (auto rotation : {-1, 1, 89, 91, 271, 360}) {
        SCOPED_TRACE(rotation);
        EXPECT_FALSE(seaside::parse_config("[monitors]\nrotation=" + std::to_string(rotation)));
    }
}

TEST_F(ConfigTest, WrongTypesUnknownFieldsAndInvalidEnumsAreRejected) {
    const std::array invalid{
        "window=1",
        "[input]\nmouse=[]",
        "[unknown]",
        "[input.keyboard]",
        "[window]\nnew_feature=true",
        "[autostart]\ncommands=1",
        "[autostart]\ncommands=[1]",
        "[autostart]\ncommands=['']",
        "[wallpaper]\npath=3",
        "[wallpaper]\npath='relative.png'",
        "[wallpaper]\nfit='contain'",
        "[window]\ntiling=1",
        "[window]\ntitlebars='false'",
        "[window]\ngaps_outer=true",
        "[window]\ngaps_inner=1.0",
        "[window]\nborder_width='2'",
        "[window]\nactive_border_color='#ffcc'",
        "[window]\ninactive_border_color='#12345z'",
        "[appearance]\ncolor_scheme='automatic'",
        "[audio]\ndefault_input=''",
        "[audio]\ndefault_output=180",
        "[power]\nprofile='power-saver'",
        "[window]\nlayout='master'",
        "[workspaces]\nmode='local'",
        "[monitors]\nprimary=1",
        "[monitors]\nprimary='DP-1'",
        "[monitors]\nresolution='0x1080'",
        "[monitors]\nresolution='1920X1080'",
        "[monitors]\nresolution='2147483648x1080'",
        "[monitors]\nrefresh_rate=0",
        "[monitors]\nrefresh_rate=-1",
        "[monitors]\nrefresh_rate='144'",
        "[monitors]\nscale=true",
        "[monitors]\nrotation=90.0",
        "[monitors]\nposition=[0]",
        "[monitors]\nposition=[false,0]",
        "[monitors]\nposition=[0,1.0]",
        "[monitors]\nposition=[2147483648,0]",
        "[monitors]\nposition='left'",
        "[monitors]\noutput={}",
        "[[monitors.output]]\nscale=1",
        "[[monitors.output]]\nmatch='connector:DP-1'\nprimary='auto'",
        "[[monitors.output]]\nmatch=1",
        "[dwindle]\ninsertion='random'",
        "[dwindle]\nsplit_ratio=true",
        "[scrolling]\ndefault_column_width='auto'",
        "[input.mouse]\nprofile='adaptive'",
        "[input.mouse]\nsensitivity=false",
        "[input.mouse]\nscroll_direction='up'",
        "[render.blur]\nstrength=1.01",
        "[render.blur]\npasses=0",
        "[render.blur]\npasses=5",
        "[idle]\ndisplay_off_after_minutes=1.0",
        "[render]\ndirect_scanout='true'",
        "[render]\nadaptive_sync=true",
        "[render]\nadaptive_sync='auto'",
        "[render]\nallow_tearing=1",
        "[render]\nfullscreen_aspect='cover'",
        "[render]\nexplicit_sync=false",
        "[night_light]\nenabled=0",
        "[night_light]\ntemperature_kelvin=4500.0",
        "keybinds=[]",
    };
    for (auto text : invalid) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(seaside::parse_config(text));
    }
    for (auto field : {"monitors.scale", "monitors.refresh_rate", "dwindle.split_ratio",
                       "scrolling.default_column_width", "input.mouse.sensitivity"}) {
        for (auto number : {"nan", "+nan", "inf", "-inf"}) {
            const std::string input = std::string(field) + "=" + number;
            SCOPED_TRACE(input);
            EXPECT_FALSE(seaside::parse_config(input));
        }
    }
}

TEST_F(ConfigTest, BindingTablesReplaceInsteadOfMergeAndRejectNormalizedDuplicates) {
    auto empty = seaside::parse_config("[keybinds]");
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->keybinds.empty());
    auto single = seaside::parse_config("[keybinds]\n'Super+B'='exec firefox'");
    ASSERT_TRUE(single);
    ASSERT_EQ(single->keybinds.size(), 1U);
    EXPECT_EQ(single->keybinds.begin()->first, seaside::parse_chord("Super+B").value());
    EXPECT_EQ(single->keybinds.begin()->second.kind, seaside::ActionKind::exec);
    EXPECT_EQ(single->keybinds.begin()->second.text, "firefox");
    EXPECT_FALSE(single->keybinds.contains(seaside::parse_chord("Super+Return").value()));
    EXPECT_FALSE(
        seaside::parse_config("[keybinds]\n'Super+Ctrl+A'='exit'\n'Ctrl+Super+A'='close_window'"));
    EXPECT_FALSE(seaside::parse_config("[keybinds]\n'Prior'='exit'\n'Page_Up'='close_window'"));
    EXPECT_EQ(seaside::parse_config("[keybinds]").value().keybinds.size(), 0U);
    EXPECT_EQ(seaside::parse_config("").value().keybinds, seaside::default_bindings());
}

TEST_F(ConfigTest, NonRepresentableIntegerToFloatConversionsAreRejected) {
    for (auto field : {"monitors.scale", "monitors.refresh_rate", "dwindle.split_ratio",
                       "scrolling.default_column_width", "input.mouse.sensitivity"}) {
        for (auto integer : {"9007199254740993", "9223372036854775807", "-9223372036854775808"}) {
            const std::string text = std::string(field) + "=" + integer;
            SCOPED_TRACE(text);
            EXPECT_FALSE(seaside::parse_config(text));
        }
    }
    for (auto field : {"scale", "refresh_rate"}) {
        SCOPED_TRACE(field);
        EXPECT_FALSE(seaside::parse_config("[[monitors.output]]\nmatch='connector:DP-1'\n" +
                                           std::string(field) + "=9007199254740993"));
    }
}

TEST_F(ConfigTest, MonitorOverridesDistinguishInheritanceFromExplicitAuto) {
    auto result = seaside::parse_config(
        "[monitors]\nresolution='1920x1080'\nrefresh_rate=144\nposition=[3,4]\nscale=1.5\nrotation="
        "90\n[[monitors.output]]\nmatch='connector:DP-1'\n[[monitors.output]]\nmatch='connector:DP-"
        "2'\nresolution='auto'\nrefresh_rate='auto'\nposition='auto'\nscale=2\nrotation=180");
    ASSERT_TRUE(result);
    ASSERT_EQ(result->monitors.output.size(), 2U);
    auto inherited =
        seaside::effective_monitor_settings(result->monitors.defaults, result->monitors.output[0]);
    EXPECT_EQ(inherited, result->monitors.defaults);
    auto automatic =
        seaside::effective_monitor_settings(result->monitors.defaults, result->monitors.output[1]);
    EXPECT_FALSE(automatic.resolution);
    EXPECT_FALSE(automatic.refresh_rate);
    EXPECT_FALSE(automatic.position);
    EXPECT_EQ(automatic.scale, 2);
    EXPECT_EQ(automatic.rotation, 180);
    EXPECT_TRUE(seaside::validate_monitor_matches(*result, {}));
}

TEST_F(ConfigTest, ConnectedIdentityAmbiguityAndAliasedDuplicatesAreRejected) {
    auto duplicate = seaside::parse_config("[[monitors.output]]\nmatch='connector:DP-1'\n[["
                                           "monitors.output]]\nmatch='identity:serial'");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(seaside::validate_monitor_matches(*duplicate, {}));
    EXPECT_FALSE(seaside::validate_monitor_matches(*duplicate, {{"serial", "DP-1"}}));
    auto ambiguous = seaside::parse_config("[monitors]\nprimary='identity:same'");
    ASSERT_TRUE(ambiguous);
    EXPECT_FALSE(
        seaside::validate_monitor_matches(*ambiguous, {{"same", "DP-1"}, {"same", "DP-2"}}));
    auto override = seaside::parse_config("[[monitors.output]]\nmatch='identity:same'");
    ASSERT_TRUE(override);
    EXPECT_FALSE(
        seaside::validate_monitor_matches(*override, {{"same", "DP-1"}, {"same", "DP-2"}}));
}

TEST_F(ConfigTest, OverrideFieldTypesRangesAndExplicitZeroValuesAreValidated) {
    const std::string prefix = "[[monitors.output]]\nmatch='connector:DP-1'\n";
    for (auto invalid : {"resolution=1", "resolution='0x1'", "refresh_rate=0", "refresh_rate=nan",
                         "refresh_rate='144'", "scale=0.49", "scale=4.01", "scale=true",
                         "rotation=1", "rotation=false", "position=[0]", "position=[0.0,0]",
                         "position=[-2147483649,0]", "position='left'"}) {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(seaside::parse_config(prefix + invalid));
    }
    auto zero = seaside::parse_config("[monitors]\nrotation=90\nposition=[100,200]\n" + prefix +
                                      "rotation=0\nposition=[0,0]\nscale=0.5");
    ASSERT_TRUE(zero);
    ASSERT_EQ(zero->monitors.output.size(), 1U);
    auto effective =
        seaside::effective_monitor_settings(zero->monitors.defaults, zero->monitors.output.front());
    EXPECT_EQ(effective.rotation, 0);
    EXPECT_EQ(effective.scale, 0.5);
    EXPECT_EQ(effective.position, (seaside::Position{0, 0}));
    EXPECT_FALSE(seaside::parse_config("[monitors]\noutput=['not a table']"));
}

TEST_F(ConfigTest, FileAndStringByteLimitsAreExact) {
    std::string exact(seaside::max_config_bytes, ' ');
    EXPECT_TRUE(seaside::parse_config(exact));
    exact += ' ';
    EXPECT_FALSE(seaside::parse_config(exact));
    auto command = [](std::size_t length) {
        return "[autostart]\ncommands=['" + std::string(length, 'x') + "']";
    };
    EXPECT_TRUE(seaside::parse_config(command(4096)));
    EXPECT_FALSE(seaside::parse_config(command(4097)));
    std::string array = "[autostart]\ncommands=[";
    for (unsigned i = 0; i < 128; ++i)
        array += "'x',";
    EXPECT_TRUE(seaside::parse_config(array + "]"));
    EXPECT_FALSE(seaside::parse_config(array + "'x']"));
    EXPECT_TRUE(seaside::parse_config("[wallpaper]\npath='/" + std::string(4095, 'x') + "'"));
    EXPECT_FALSE(seaside::parse_config("[wallpaper]\npath='/" + std::string(4096, 'x') + "'"));
}

TEST_F(ConfigTest, NestedParserFailuresRetainSourceAndFieldDiagnostics) {
    struct Case {
        std::string_view text;
        std::string_view field;
    };
    const std::array cases{
        Case{"# source marker\n[[monitors.output]]\nmatch='invalid:DP-1'", "monitors.output.match"},
        Case{"# source marker\n[keybinds]\n'A'=7", "keybinds"},
        Case{"# source marker\n[keybinds]\n'Fn+F12'='exit'", "keybinds"},
        Case{"# source marker\n[keybinds]\n'A'='unknown_action'", "keybinds"},
    };
    for (const auto &[text, field] : cases) {
        SCOPED_TRACE(text);
        auto result = seaside::parse_config(text, "nested-errors.toml");
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().path, "nested-errors.toml");
        EXPECT_EQ(result.error().field, field);
        EXPECT_EQ(result.error().line, 3U);
        EXPECT_GT(result.error().column, 0U);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST_F(ConfigTest, MalformedUtf8NulAndUnreleasedSyntaxFailWithBoundedDiagnostics) {
    const std::array invalid{
        std::string("[window"),
        std::string("window.tiling=true\nwindow.tiling=false"),
        std::string("[wallpaper]\npath=\"/bad\\u0000.png\""),
        std::string("[autostart]\ncommands=[\"bad\\u0000command\"]"),
        std::string("[wallpaper]\npath=\"/\xff.png\""),
        std::string("window.layout=\"\\e\""),
    };
    for (const auto &text : invalid) {
        auto result = seaside::parse_config(text, "custom.toml");
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().path, "custom.toml");
        EXPECT_GT(result.error().line, 0U);
        EXPECT_LE(result.error().field.size(), 256U);
        EXPECT_LE(result.error().message.size(), 512U);
    }
    auto field = seaside::parse_config("[window]\ngaps_outer=true", "custom.toml");
    ASSERT_FALSE(field);
    EXPECT_EQ(field.error().field, "window.gaps_outer");
    EXPECT_EQ(field.error().line, 2U);
    auto huge = seaside::parse_config(std::string(10000, 'x') + "=1", std::string(5000, 'p'));
    ASSERT_FALSE(huge);
    EXPECT_LE(huge.error().path.size(), 4096U);
    EXPECT_LE(huge.error().field.size(), 256U);
    std::string unicode;
    for (unsigned i = 0; i < 2000; ++i)
        unicode += "字";
    auto unicode_error = seaside::parse_config("'" + unicode + "'=1", unicode);
    ASSERT_FALSE(unicode_error);
    EXPECT_EQ(unicode_error.error().field.size() % 3, 0U);
    EXPECT_EQ(unicode_error.error().path.size() % 3, 0U);
}

TEST_F(ConfigTest, NestedValuesAndDottedTablesAreBoundedWithoutInspectingStrings) {
    EXPECT_FALSE(seaside::parse_config("x=" + std::string(10000, '[') + std::string(10000, ']')));
    std::string path = "[";
    for (unsigned i = 0; i < 100000; ++i)
        path += "a.";
    EXPECT_FALSE(seaside::parse_config(path + "a]"));
    EXPECT_FALSE(seaside::parse_config("autostart.commands=['''x'''' ]\n" + path + "a]"));
    EXPECT_FALSE(seaside::parse_config("autostart.commands=[\"\"\"x\"\"\"\"\" ]\n" + path + "a]"));
    EXPECT_FALSE(seaside::parse_config("x={" + path.substr(1) + "a=1}"));
    std::string compound = "x=";
    for (unsigned level = 0; level < 100; ++level) {
        compound += "{a.a.a.a.a.a.a.a=[\n";
    }
    compound += "1";
    for (unsigned level = 0; level < 100; ++level)
        compound += "]}";
    EXPECT_FALSE(seaside::parse_config(compound));
    EXPECT_TRUE(seaside::parse_config("# " + std::string(10000, '.') + "\n[wallpaper]\npath='/" +
                                      std::string(1000, '.') + "'"));
    EXPECT_TRUE(seaside::parse_config("[autostart]\ncommands=['''echo\n" + std::string(1000, '.') +
                                      "''']"));
}

TEST_F(ConfigTest, SeededMalformedCorpusNeverProducesUnboundedDiagnostics) {
    constexpr unsigned seed = 0x5EA512;
    RecordProperty("seed", std::to_string(seed));
    SCOPED_TRACE(seed);
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 300; ++trial) {
        std::string input;
        for (unsigned i = 0, length = random() % 512; i < length; ++i)
            input += static_cast<char>(random() & 0xffU);
        std::string hex;
        constexpr char digits[] = "0123456789abcdef";
        for (unsigned char byte : input) {
            hex += digits[byte >> 4U];
            hex += digits[byte & 0xfU];
        }
        SCOPED_TRACE("trial=" + std::to_string(trial) + " input_hex=" + hex);
        auto result = seaside::parse_config(input, "seeded.toml");
        if (!result) {
            EXPECT_LE(result.error().message.size(), 512U) << "seed=" << seed << " trial=" << trial;
            EXPECT_LE(result.error().field.size(), 256U);
        }
    }
}
} // namespace
