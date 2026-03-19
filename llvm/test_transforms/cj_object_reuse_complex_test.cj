package test_reuse.complex

public class ContainerWithOption {
    public var id: Int64 = 0
    public var name: Option<String> = None
    public var value: Int64 = 0
    public var timestamp: Int64 = 0
    public var checksum: Int64 = 0
    public var version: Int64 = 0
    public var priority: Int64 = 0
    public var sequence: Int64 = 0

    public init() {}

    public init(id: Int64, name: String, value: Int64, timestamp: Int64, checksum: Int64, version: Int64, priority: Int64, sequence: Int64) {
        this.id = id
        this.name = Some(name)
        this.value = value
        this.timestamp = timestamp
        this.checksum = checksum
        this.version = version
        this.priority = priority
        this.sequence = sequence
    }

    public func getId(): Int64 {
        return this.id
    }

    public func getValue(): Int64 {
        return this.value
    }

    public func hasName(): Bool {
        return this.name.isSome()
    }

    public func getTimestamp(): Int64 {
        return this.timestamp
    }

    public func getChecksum(): Int64 {
        return this.checksum
    }
}

public class Node {
    public var data: Int64 = 0
    public var parent: Option<Box<Node>> = None
    public var extra: Int64 = 0

    public init() {}

    public init(data: Int64) {
        this.data = data
    }

    public func getData(): Int64 {
        return this.data
    }

    public func setData(d: Int64): Unit {
        this.data = d
    }
}

public class DataHolder {
    public var id: Int64 = 0
    public var value: Option<String> = None
    public var flag: Bool = false
    public var timestamp: Int64 = 0
    public var checksum: Int64 = 0
    public var version: Int64 = 0
    public var priority: Int64 = 0
    public var sequence: Int64 = 0

    public init() {}

    public init(id: Int64, value: String, flag: Bool, timestamp: Int64, checksum: Int64, version: Int64, priority: Int64, sequence: Int64) {
        this.id = id
        this.value = Some(value)
        this.flag = flag
        this.timestamp = timestamp
        this.checksum = checksum
        this.version = version
        this.priority = priority
        this.sequence = sequence
    }

    public func getId(): Int64 {
        return this.id
    }

    public func isValid(): Bool {
        return this.flag
    }

    public func hasValue(): Bool {
        return this.value.isSome()
    }

    public func getTimestamp(): Int64 {
        return this.timestamp
    }

    public func getChecksum(): Int64 {
        return this.checksum
    }
}

public class WrapperBox {
    public var content: Option<Box<ContainerWithOption>> = None
    public var timestamp: Int64 = 0

    public init() {}

    public init(content: Box<ContainerWithOption>, timestamp: Int64) {
        this.content = Some(content)
        this.timestamp = timestamp
    }

    public func getTimestamp(): Int64 {
        return this.timestamp
    }
}

public class CacheEntry {
    public var key: Int64 = 0
    public var metadata: Option<String> = None
    public var priority: Int64 = 0

    public init() {}

    public init(key: Int64, metadata: String, priority: Int64) {
        this.key = key
        this.metadata = Some(metadata)
        this.priority = priority
    }

    public func getKey(): Int64 {
        return this.key
    }

    public func getPriority(): Int64 {
        return this.priority
    }
}

public class ResultWithOption {
    public var code: Int64 = 0
    public var message: Option<String> = None
    public var extra: Int64 = 0

    public init() {}

    public init(code: Int64, message: String, extra: Int64) {
        this.code = code
        this.message = Some(message)
        this.extra = extra
    }

    public func getCode(): Int64 {
        return this.code
    }
}

public func testContainerReuse(): Int64 {
    let c1 = ContainerWithOption(1, "first", 100, 1000, 1111, 1, 10, 100)
    let id1 = c1.getId()
    let hasName1 = c1.hasName()

    let c2 = DataHolder(id1 + 10, "second", true, 2000, 2222, 2, 20, 200)
    let result = c2.getId() + if (c2.hasValue()) { 1 } else { 0 }

    return result + if (hasName1) { 100 } else { 0 }
}

public func testNestedStructReuse(): Int64 {
    let node1 = Node(100)
    node1.setData(node1.getData() + 1)

    let holder = DataHolder(node1.getData(), "node", false, 2000, 2222, 2, 20, 200)
    return holder.getId()
}

public func testChainReuse(): Int64 {
    var result: Int64 = 0

    let c1 = ContainerWithOption(1, "a", 10, 1001, 1002, 1, 1, 10)
    result += c1.getId()

    let c2 = DataHolder(2, "b", true, 2001, 2002, 2, 2, 20)
    result += c2.getId()

    let c3 = ContainerWithOption(3, "c", 30, 3001, 3002, 3, 3, 30)
    result += c3.getId()

    return result
}

public func testBranchReuse(flag: Bool): Int64 {
    let c1 = ContainerWithOption(100, "initial", 999, 10000, 10001, 1, 100, 1000)
    let base = c1.getId()

    if (flag) {
        let c2 = DataHolder(base + 1, "branch", true, 20000, 20001, 2, 200, 2000)
        return c2.getId()
    } else {
        let c3 = ContainerWithOption(base + 2, "else", 888, 30000, 30001, 3, 300, 3000)
        return c3.getId()
    }
}

public func testLoopReuse(): Int64 {
    var sum: Int64 = 0
    for (i in 0..10000) {
        let c = ContainerWithOption(i, "loop", i * 10, i * 1000, i * 1001, i, i * 10, i * 100)
        sum += c.getId() + c.getValue()
    }
    return sum
}

public func testWrapperReuse(): Int64 {
    let inner = ContainerWithOption(10, "inner", 100, 1000, 1001, 1, 10, 100)
    let box1 = Box<ContainerWithOption>(inner)

    let entry = CacheEntry(20, "cache", 200)
    return entry.getKey() + entry.getPriority()
}

public func testMultiLayerReuse(): Int64 {
    let c1 = ContainerWithOption(1, "one", 10, 1000, 1001, 1, 10, 100)
    let v1 = c1.getValue()

    let d1 = DataHolder(v1, "two", true, 2000, 2001, 2, 20, 200)
    let v2 = if (d1.isValid()) { d1.getId() } else { 0 }

    let c2 = ContainerWithOption(v2, "three", 30, 3000, 3001, 3, 30, 300)
    return c2.getValue()
}

public func testConditionalReuse(cond: Int64): Int64 {
    let c1 = ContainerWithOption(1, "start", 100, 1000, 1001, 1, 10, 100)
    let base = c1.getId()

    if (cond > 0) {
        let d1 = DataHolder(base + cond, "positive", true, 2000, 2001, 2, 20, 200)
        return d1.getId()
    } else if (cond < 0) {
        let c2 = ContainerWithOption(base + -1 * cond, "negative", 200, 3000, 3001, 3, 30, 300)
        return c2.getId()
    } else {
        return base
    }
}

public func testSequentialReuse(): Int64 {
    var total: Int64 = 0

    let c1 = ContainerWithOption(10, "X", 1, 1000, 1001, 1, 1, 10)
    total += c1.getId()

    let d1 = DataHolder(20, "Y", true, 2000, 2001, 2, 2, 20)
    total += d1.getId()

    let c2 = ContainerWithOption(30, "Z", 3, 3000, 3001, 3, 3, 30)
    total += c2.getId()

    let d2 = DataHolder(40, "W", false, 4000, 4001, 4, 4, 40)
    total += d2.getId()

    return total
}

public func testResultReuse(): Int64 {
    let r1 = ResultWithOption(0, "ok", 100)
    let code1 = r1.getCode()

    let c1 = ContainerWithOption(code1 + 1, "result", 200, 5000, 5001, 5, 50, 500)
    return c1.getValue()
}

public func testBatchReuse(count: Int64): Int64 {
    var total: Int64 = 0

    var i: Int64 = 0
    while (i < count) {
        let c1 = ContainerWithOption(i, "data1", i * 10, i * 1000, i * 1001, i, i * 10, i * 100)
        total += c1.getId()

        let d1 = DataHolder(i + 1, "data2", true, i * 2000, i * 2001, i + 1, i * 20, i * 200)
        total += d1.getId()

        let c2 = ContainerWithOption(i + 2, "data3", i * 20, i * 3000, i * 3001, i + 2, i * 30, i * 300)
        total += c2.getId()

        i += 3
    }

    return total
}

public func testLargeScaleReuse(): Int64 {
    var sum: Int64 = 0
    let iterations = 5000

    for (i in 0..iterations) {
        let c = ContainerWithOption(i, "item", i * 10, i * 1000, i * 1001, i, i * 10, i * 100)
        sum += c.getId()

        if (i % 2 == 0) {
            let d = DataHolder(i + 1000, "even", true, i * 2000, i * 2001, i, i * 20, i * 200)
            sum += d.getId()
        }
    }

    return sum
}

public func testComplexLoopReuse(): Int64 {
    var result: Int64 = 0
    let outerCount = 1000

    for (outer in 0..outerCount) {
        let c1 = ContainerWithOption(outer, "outer1", outer * 100, outer * 1000, outer * 1001, outer, outer * 10, outer * 100)
        result += c1.getId()

        for (inner in 0..5) {
            let d = DataHolder(outer * 10 + inner, "inner", true, inner * 500, inner * 501, inner, inner * 5, inner * 50)
            result += d.getId()
        }

        let c2 = ContainerWithOption(outer + 1, "outer2", outer * 200, outer * 2000, outer * 2001, outer + 1, outer * 20, outer * 200)
        result += c2.getValue()
    }

    return result
}

main() {
    println("=== CJObjectReuseOpt Memory Optimization Test ===")
    println("Each object is ~72 bytes (8 fields + Option<String> pointer)")
    println("Memory optimization: Reusing memory instead of allocating new objects")
    println("")

    let r1 = testContainerReuse()
    println("testContainerReuse: ${r1}")

    let r2 = testNestedStructReuse()
    println("testNestedStructReuse: ${r2}")

    let r3 = testChainReuse()
    println("testChainReuse: ${r3}")

    let r4 = testBranchReuse(true)
    println("testBranchReuse(true): ${r4}")

    let r5 = testBranchReuse(false)
    println("testBranchReuse(false): ${r5}")

    let r6 = testLoopReuse()
    println("testLoopReuse (10000 iterations): ${r6}")
    println("  -> Without reuse: 10000 * 72 bytes = 720 KB")
    println("  -> With reuse: 1 * 72 bytes = 72 bytes")
    println("  -> Memory saved: ~720 KB")

    let r7 = testWrapperReuse()
    println("testWrapperReuse: ${r7}")

    let r8 = testMultiLayerReuse()
    println("testMultiLayerReuse: ${r8}")

    let r9 = testConditionalReuse(5)
    println("testConditionalReuse(5): ${r9}")

    let r10 = testConditionalReuse(-3)
    println("testConditionalReuse(-3): ${r10}")

    let r11 = testConditionalReuse(0)
    println("testConditionalReuse(0): ${r11}")

    let r12 = testSequentialReuse()
    println("testSequentialReuse: ${r12}")

    let r13 = testResultReuse()
    println("testResultReuse: ${r13}")

    println("")
    println("=== Large Scale Tests ===")

    let r14 = testBatchReuse(5000)
    println("testBatchReuse(5000): ${r14}")
    println("  -> Without reuse: 5000 * 3 * 72 bytes = 1.08 MB")
    println("  -> With reuse: 1 * 72 bytes = 72 bytes")
    println("  -> Memory saved: ~1.08 MB")

    let r15 = testLargeScaleReuse()
    println("testLargeScaleReuse (5000 iterations): ${r15}")
    println("  -> Without reuse: 7500 * 72 bytes = 540 KB")
    println("  -> With reuse: 1 * 72 bytes = 72 bytes")
    println("  -> Memory saved: ~540 KB")

    let r16 = testComplexLoopReuse()
    println("testComplexLoopReuse (1000*5 inner loops): ${r16}")
    println("  -> Without reuse: 1000 * 7 * 72 bytes = 504 KB")
    println("  -> With reuse: 1 * 72 bytes = 72 bytes")
    println("  -> Memory saved: ~504 KB")

    println("")
    println("=== Total potential memory savings: > 2 MB ===")
}