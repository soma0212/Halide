#include "Halide.h"

#include <cstdio>
#ifdef TEST_WITH_SERIALIZATION
#include <map>
#endif
#include <set>
#include <string>
#ifdef TEST_WITH_SERIALIZATION
#include <vector>
#endif

using namespace Halide;
using namespace Halide::Internal;

namespace {

class CheckStreamingAccesses : public IRMutator {
public:
    using IRMutator::mutate;

    Stmt mutate(const Stmt &s) override {
        class Finder : public IRVisitor {
        public:
            std::set<std::string> streaming_loads;
            std::set<std::string> streaming_stores;
            std::set<std::string> ordinary_loads;
            std::set<std::string> ordinary_stores;

        protected:
            using IRVisitor::visit;

            void visit(const Load *op) override {
                (op->is_streaming ? streaming_loads : ordinary_loads).insert(op->name);
                IRVisitor::visit(op);
            }

            void visit(const Store *op) override {
                (op->is_streaming ? streaming_stores : ordinary_stores).insert(op->name);
                IRVisitor::visit(op);
            }
        } finder;

        s.accept(&finder);

        auto require = [](const std::set<std::string> &names, const std::string &name, const char *kind) {
            if (!names.count(name)) {
                std::fprintf(stderr, "Expected a %s access to %s\n", kind, name.c_str());
                std::fprintf(stderr, "Found:");
                for (const auto &found : names) {
                    std::fprintf(stderr, " %s", found.c_str());
                }
                std::fprintf(stderr, "\n");
                std::exit(1);
            }
        };

        require(finder.streaming_loads, "streaming_input", "streaming load");
        require(finder.streaming_loads, "streaming_producer", "streaming load");
        require(finder.streaming_stores, "streaming_producer", "streaming store");
        require(finder.streaming_stores, "output", "streaming store");
        require(finder.ordinary_loads, "ordinary_input", "ordinary load");
        require(finder.ordinary_loads, "ordinary_producer", "ordinary load");
        require(finder.ordinary_stores, "ordinary_producer", "ordinary store");

        return s;
    }
};

}  // namespace

int main(int argc, char **argv) {
    Var x;
    ImageParam streaming_input(Int(32), 1, "streaming_input");
    ImageParam ordinary_input(Int(32), 1, "ordinary_input");
    Func streaming_producer("streaming_producer");
    Func ordinary_producer("ordinary_producer");
    Func output("output");

    streaming_producer(x) = streaming_input(x) + 1;
    ordinary_producer(x) = ordinary_input(x) + 2;
    output(x) = streaming_producer(x) + ordinary_producer(x);

    streaming_input.stream_loads();
    streaming_producer.compute_root().stream_loads().stream_stores();
    ordinary_producer.compute_root();
    output.stream_stores();

    streaming_producer.vectorize(x, 8);
    ordinary_producer.vectorize(x, 8);
    output.vectorize(x, 8);

    constexpr int size = 1024;
    Buffer<int32_t> a(size), b(size);
    for (int i = 0; i < size; i++) {
        a(i) = i;
        b(i) = 3 * i;
    }
    streaming_input.set(a);
    ordinary_input.set(b);

    Pipeline pipeline(output);
#ifdef TEST_WITH_SERIALIZATION
    // Verify that the access directives survive pipeline serialization as
    // well as the normal lowering pipeline.
    std::vector<uint8_t> serialized;
    std::map<std::string, Parameter> params;
    serialize_pipeline(pipeline, serialized, params);
    pipeline = deserialize_pipeline(serialized, params);
#endif
    pipeline.add_custom_lowering_pass(new CheckStreamingAccesses);

    Buffer<int32_t> result = pipeline.realize({size});
    for (int i = 0; i < size; i++) {
        if (result(i) != 4 * i + 3) {
            std::fprintf(stderr, "Incorrect result at %d: %d\n", i, result(i));
            return 1;
        }
    }

    std::printf("Success!\n");
    return 0;
}
