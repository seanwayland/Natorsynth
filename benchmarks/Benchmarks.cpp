#include "PluginEditor.h"
#include "catch2/benchmark/catch_benchmark_all.hpp"
#include "catch2/catch_test_macros.hpp"

#include <memory>

TEST_CASE ("Boot performance")
{
    BENCHMARK_ADVANCED ("Editor open and close")
    (Catch::Benchmark::Chronometer meter)
    {
        auto plugin = std::make_unique<NatorsynthAudioProcessor>();

        // due to complex construction logic of the editor, let's measure open/close together
        meter.measure ([&] (int /* i */) {
            auto editor = plugin->createEditorIfNeeded();
            plugin->editorBeingDeleted (editor);
            delete editor;
            return plugin->getActiveEditor();
        });
    };
}
