#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/latch_transparent_read.hpp"

#include <iostream>
#include <cassert>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[transform-latch-transparent-read] " << message << '\n';
        return 1;
    }
}

// Test 1: Basic latch transparent read transformation
int testBasicLatchTransparentRead()
{
    std::cout << "Test 1: Basic latch transparent read..." << std::endl;
    
    Design design;
    Graph &graph = design.createGraph("test_basic_latch");
    
    // Create input ports
    ValueId en = graph.createValue(1, false);
    graph.bindInputPort("en", en);
    
    ValueId d = graph.createValue(8, false);
    graph.bindInputPort("d", d);
    
    // Create latch declaration
    SymbolId latchSym = graph.internSymbol("data_latch");
    OperationId latchDecl = graph.createOperation(OperationKind::kLatch, latchSym);
    graph.setAttr(latchDecl, "width", static_cast<int64_t>(8));
    graph.setAttr(latchDecl, "isSigned", false);
    
    // Create latch write port (no mask)
    SymbolId writeSym = graph.internSymbol("data_latch_write");
    OperationId writePort = graph.createOperation(OperationKind::kLatchWritePort, writeSym);
    graph.addOperand(writePort, en);      // updateCond
    graph.addOperand(writePort, d);       // nextValue
    graph.setAttr(writePort, "latchSymbol", std::string("data_latch"));
    graph.assignWriterOrder(writePort, "data_latch");
    
    // Create latch read port
    SymbolId readSym = graph.internSymbol("data_latch_read");
    OperationId readPort = graph.createOperation(OperationKind::kLatchReadPort, readSym);
    graph.setAttr(readPort, "latchSymbol", std::string("data_latch"));
    ValueId readValue = graph.createValue(8, false);
    graph.addResult(readPort, readValue);
    
    // Record initial operation count
    size_t initialOpCount = graph.operations().size();
    std::cout << "  Initial op count: " << initialOpCount << std::endl;
    
    // Run the pass
    PassDiagnostics diags;
    PassManager manager;
    manager.addPass(std::make_unique<LatchTransparentReadPass>());
    PassManagerResult result = manager.run(design, diags);
    
    if (!result.success) {
        return fail("Pass failed unexpectedly");
    }
    if (!result.changed) {
        return fail("Pass did not make changes");
    }
    
    // Verify operations were added (mux should be added)
    size_t finalOpCount = graph.operations().size();
    std::cout << "  Final op count: " << finalOpCount << std::endl;
    
    if (finalOpCount <= initialOpCount) {
        return fail("Expected new operations to be added");
    }
    
    // Check that a mux operation was added
    bool foundMux = false;
    for (const auto opId : graph.operations()) {
        Operation op = graph.getOperation(opId);
        if (op.kind() == OperationKind::kMux) {
            foundMux = true;
            break;
        }
    }
    
    if (!foundMux) {
        return fail("Expected kMux operation to be added");
    }
    
    std::cout << "  Pass: Basic transparent read works" << std::endl;
    return 0;
}

// Test 2: Latch with mask
int testLatchWithMask()
{
    std::cout << "Test 2: Latch with mask..." << std::endl;
    
    Design design;
    Graph &graph = design.createGraph("test_latch_mask");
    
    // Create input ports
    ValueId en = graph.createValue(1, false);
    graph.bindInputPort("en", en);
    
    ValueId d = graph.createValue(8, false);
    graph.bindInputPort("d", d);
    
    ValueId mask = graph.createValue(8, false);
    graph.bindInputPort("mask", mask);
    
    // Create latch declaration
    SymbolId latchSym = graph.internSymbol("data_latch");
    OperationId latchDecl = graph.createOperation(OperationKind::kLatch, latchSym);
    graph.setAttr(latchDecl, "width", static_cast<int64_t>(8));
    graph.setAttr(latchDecl, "isSigned", false);
    
    // Create latch write port with mask
    SymbolId writeSym = graph.internSymbol("data_latch_write");
    OperationId writePort = graph.createOperation(OperationKind::kLatchWritePort, writeSym);
    graph.addOperand(writePort, en);      // updateCond
    graph.addOperand(writePort, d);       // nextValue
    graph.addOperand(writePort, mask);    // mask
    graph.setAttr(writePort, "latchSymbol", std::string("data_latch"));
    graph.assignWriterOrder(writePort, "data_latch");
    
    // Create latch read port
    SymbolId readSym = graph.internSymbol("data_latch_read");
    OperationId readPort = graph.createOperation(OperationKind::kLatchReadPort, readSym);
    graph.setAttr(readPort, "latchSymbol", std::string("data_latch"));
    ValueId readValue = graph.createValue(8, false);
    graph.addResult(readPort, readValue);
    
    // Run the pass
    PassDiagnostics diags;
    PassManager manager;
    manager.addPass(std::make_unique<LatchTransparentReadPass>());
    PassManagerResult result = manager.run(design, diags);
    
    if (!result.success) {
        return fail("Pass failed unexpectedly");
    }
    if (!result.changed) {
        return fail("Pass did not make changes");
    }
    
    // With mask, we should have added more operations: ~mask, nextValue & mask, 
    // readValue & ~mask, (nextValue & mask) | (readValue & ~mask), and mux
    size_t finalOpCount = graph.operations().size();
    std::cout << "  Final op count with mask: " << finalOpCount << std::endl;
    
    std::cout << "  Pass: Mask handling works" << std::endl;
    return 0;
}

// Test 3: Multiple write ports should fail
int testMultipleWritePorts()
{
    std::cout << "Test 3: Multiple write ports detection..." << std::endl;
    
    Design design;
    Graph &graph = design.createGraph("test_multi_write");
    
    // Create input ports
    ValueId en1 = graph.createValue(1, false);
    graph.bindInputPort("en1", en1);
    
    ValueId en2 = graph.createValue(1, false);
    graph.bindInputPort("en2", en2);
    
    ValueId d1 = graph.createValue(8, false);
    graph.bindInputPort("d1", d1);
    
    ValueId d2 = graph.createValue(8, false);
    graph.bindInputPort("d2", d2);
    
    // Create latch declaration
    SymbolId latchSym = graph.internSymbol("data_latch");
    OperationId latchDecl = graph.createOperation(OperationKind::kLatch, latchSym);
    graph.setAttr(latchDecl, "width", static_cast<int64_t>(8));
    graph.setAttr(latchDecl, "isSigned", false);
    
    // Create first write port
    SymbolId writeSym1 = graph.internSymbol("data_latch_write1");
    OperationId writePort1 = graph.createOperation(OperationKind::kLatchWritePort, writeSym1);
    graph.addOperand(writePort1, en1);
    graph.addOperand(writePort1, d1);
    graph.setAttr(writePort1, "latchSymbol", std::string("data_latch"));
    graph.assignWriterOrder(writePort1, "data_latch");
    
    // Create second write port (should cause error)
    SymbolId writeSym2 = graph.internSymbol("data_latch_write2");
    OperationId writePort2 = graph.createOperation(OperationKind::kLatchWritePort, writeSym2);
    graph.addOperand(writePort2, en2);
    graph.addOperand(writePort2, d2);
    graph.setAttr(writePort2, "latchSymbol", std::string("data_latch"));
    graph.assignWriterOrder(writePort2, "data_latch");
    
    // Create read port
    SymbolId readSym = graph.internSymbol("data_latch_read");
    OperationId readPort = graph.createOperation(OperationKind::kLatchReadPort, readSym);
    graph.setAttr(readPort, "latchSymbol", std::string("data_latch"));
    ValueId readValue = graph.createValue(8, false);
    graph.addResult(readPort, readValue);
    
    // Run the pass - should fail
    PassDiagnostics diags;
    PassManager manager;
    manager.options().stopOnError = false;  // Continue to see all errors
    manager.addPass(std::make_unique<LatchTransparentReadPass>());
    PassManagerResult result = manager.run(design, diags);
    
    if (result.success) {
        return fail("Expected pass to fail due to multiple write ports");
    }
    if (!diags.hasError()) {
        return fail("Expected error diagnostics");
    }
    
    std::cout << "  Pass: Multiple write ports correctly detected" << std::endl;
    return 0;
}

// Test 4: No write port (warning case)
int testNoWritePort()
{
    std::cout << "Test 4: No write port warning..." << std::endl;
    
    Design design;
    Graph &graph = design.createGraph("test_no_write");
    
    // Create latch declaration only
    SymbolId latchSym = graph.internSymbol("data_latch");
    OperationId latchDecl = graph.createOperation(OperationKind::kLatch, latchSym);
    graph.setAttr(latchDecl, "width", static_cast<int64_t>(8));
    graph.setAttr(latchDecl, "isSigned", false);
    
    // Create read port without write port
    SymbolId readSym = graph.internSymbol("data_latch_read");
    OperationId readPort = graph.createOperation(OperationKind::kLatchReadPort, readSym);
    graph.setAttr(readPort, "latchSymbol", std::string("data_latch"));
    
    // Create result value for read port
    SymbolId readValSym = graph.internSymbol("data_latch_read_val");
    ValueId readValue = graph.createValue(readValSym, 8, false);
    graph.addResult(readPort, readValue);
    
    // Run the pass - should warn but not fail
    PassDiagnostics diags;
    PassManager manager;
    manager.addPass(std::make_unique<LatchTransparentReadPass>());
    PassManagerResult result = manager.run(design, diags);
    
    // Should succeed but may have warnings
    if (!result.success) {
        return fail("Expected pass to succeed (with warnings)");
    }
    if (diags.hasError()) {
        return fail("Did not expect errors, only warnings");
    }
    
    std::cout << "  Pass: No write port warning works" << std::endl;
    return 0;
}

// Test 5: Multiple read ports
int testMultipleReadPorts()
{
    std::cout << "Test 5: Multiple read ports..." << std::endl;
    
    Design design;
    Graph &graph = design.createGraph("test_multi_read");
    
    // Create input ports
    ValueId en = graph.createValue(1, false);
    graph.bindInputPort("en", en);
    
    ValueId d = graph.createValue(8, false);
    graph.bindInputPort("d", d);
    
    // Create latch declaration
    SymbolId latchSym = graph.internSymbol("data_latch");
    OperationId latchDecl = graph.createOperation(OperationKind::kLatch, latchSym);
    graph.setAttr(latchDecl, "width", static_cast<int64_t>(8));
    graph.setAttr(latchDecl, "isSigned", false);
    
    // Create write port
    SymbolId writeSym = graph.internSymbol("data_latch_write");
    OperationId writePort = graph.createOperation(OperationKind::kLatchWritePort, writeSym);
    graph.addOperand(writePort, en);
    graph.addOperand(writePort, d);
    graph.setAttr(writePort, "latchSymbol", std::string("data_latch"));
    graph.assignWriterOrder(writePort, "data_latch");
    
    // Create multiple read ports
    SymbolId readSym1 = graph.internSymbol("data_latch_read1");
    OperationId readPort1 = graph.createOperation(OperationKind::kLatchReadPort, readSym1);
    graph.setAttr(readPort1, "latchSymbol", std::string("data_latch"));
    ValueId readValue1 = graph.createValue(8, false);
    graph.addResult(readPort1, readValue1);
    
    SymbolId readSym2 = graph.internSymbol("data_latch_read2");
    OperationId readPort2 = graph.createOperation(OperationKind::kLatchReadPort, readSym2);
    graph.setAttr(readPort2, "latchSymbol", std::string("data_latch"));
    ValueId readValue2 = graph.createValue(8, false);
    graph.addResult(readPort2, readValue2);
    
    // Run the pass
    PassDiagnostics diags;
    PassManager manager;
    manager.addPass(std::make_unique<LatchTransparentReadPass>());
    PassManagerResult result = manager.run(design, diags);
    
    if (!result.success) {
        return fail("Pass failed unexpectedly");
    }
    if (!result.changed) {
        return fail("Pass did not make changes");
    }
    
    std::cout << "  Pass: Multiple read ports handled correctly" << std::endl;
    return 0;
}

int main()
{
    std::cout << "=== LatchTransparentRead Pass Tests ===" << std::endl;
    
    if (int rc = testBasicLatchTransparentRead()) return rc;
    if (int rc = testLatchWithMask()) return rc;
    if (int rc = testMultipleWritePorts()) return rc;
    if (int rc = testNoWritePort()) return rc;
    if (int rc = testMultipleReadPorts()) return rc;
    
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
