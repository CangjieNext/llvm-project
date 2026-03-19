package test_reuse

public class Coord {
    public var x: Int64 = 0
    public var y: Int64 = 0

    public init() {}

    public init(x: Int64, y: Int64) {
        this.x = x
        this.y = y
    }

    public func getX(): Int64 {
        return this.x
    }

    public func getY(): Int64 {
        return this.y
    }

    public func setX(x: Int64): Unit {
        this.x = x
    }

    public func setY(y: Int64): Unit {
        this.y = y
    }

    public func sum(): Int64 {
        return this.x + this.y
    }
}

public class Pixel {
    public var x: Int64 = 0
    public var y: Int64 = 0

    public init() {}

    public init(x: Int64, y: Int64) {
        this.x = x
        this.y = y
    }

    public func getX(): Int64 {
        return this.x
    }

    public func getY(): Int64 {
        return this.y
    }

    public func product(): Int64 {
        return this.x * this.y
    }
}

public func testObjectReuseScenario(): Int64 {
    let p1 = Coord(10, 20)
    let x1 = p1.getX()
    let y1 = p1.getY()
    p1.setX(100)
    p1.setY(200)

    let p2 = Pixel(x1 + y1, x1 - y1)
    let x2 = p2.getX()
    let y2 = p2.getY()
    let product = p2.product()

    return x2 + y2 + product
}

public func testMultipleReuse(): Int64 {
    var result: Int64 = 0

    let p1 = Coord(1, 2)
    result += p1.getX()

    let p2 = Pixel(3, 4)
    result += p2.getY()

    let p3 = Coord(5, 6)
    result += p3.getX() + p3.getY()

    let p4 = Pixel(7, 8)
    result += p4.getX()

    return result
}

public func testNestedReuse(): Int64 {
    let p1 = Coord(10, 20)
    let v1 = p1.sum()

    if (v1 > 0) {
        let p2 = Pixel(v1, v1 * 2)
        return p2.getX() + p2.getY()
    }

    return v1
}

main() {
    let r1 = testObjectReuseScenario()
    println("testObjectReuseScenario result: ${r1}")

    let r2 = testMultipleReuse()
    println("testMultipleReuse result: ${r2}")

    let r3 = testNestedReuse()
    println("testNestedReuse result: ${r3}")
}