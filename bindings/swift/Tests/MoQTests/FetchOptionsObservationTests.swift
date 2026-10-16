import Testing
import CMoQCore
@testable import MoQ

@Suite("FETCH options observations")
struct FetchOptionsObservationTests {
    private struct MissingFetch: Error {}

    private func extract(_ pointer: UnsafeRawPointer?, detail: Int? = nil) throws -> FetchRequestOptions? {
        var event = moq_event_t()
        event.kind = MOQ_EVENT_FETCH_REQUEST
        event.detail_size = UInt32(detail ?? MemoryLayout<moq_fetch_request_event_t>.size)
        event.u.fetch_request.options = pointer?.assumingMemoryBound(to: moq_fetch_request_options_t.self)
        guard case .fetchRequest(let value) = ExtendedEvent.from(&event) else { throw MissingFetch() }
        return value.options
    }

    @Test("Physical v0, partial tail, current and future records are copied",
          arguments: [64, 65, 71, 72, 88])
    func copiedPrefix(_ size: Int) throws {
        #expect(MOQ_FETCH_REQUEST_OPTIONS_V0_SIZE == 64)
        #expect(MemoryLayout<moq_fetch_request_options_t>.offset(of: \.range_unresolved) == 64)
        let forms: [LocationFilter] = [.none, .relativeGroup(offset: .max),
            .absoluteStart(group: 2, object: 3), .groupEnd(startGroup: 2, startObject: 3, endGroup: 7),
            .objectEnd(startGroup: 2, startObject: 3, endGroup: 7, endObject: .max),
            .nextObject, .other(99)]
        for form in forms {
            for properties in [false, true] {
                for timeout: UInt64? in [nil, 0, .max] {
                    var options = moq_fetch_request_options_t()
                    options.struct_size = UInt32(size)
                    form.fill(&options.location)
                    options.flags = properties ? UInt32(MOQ_FETCH_REQUEST_INCLUDE_PROPERTIES) : 0
                    if timeout != nil { options.flags |= UInt32(MOQ_FETCH_REQUEST_HAS_FILL_TIMEOUT) }
                    options.fill_timeout_ms = timeout ?? .max
                    options.range_unresolved = true
                    let storage = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
                    storage.initializeMemory(as: UInt8.self, repeating: 0xa5, count: size)
                    withUnsafeBytes(of: options) {
                        storage.copyMemory(from: $0.baseAddress!, byteCount: min(size, $0.count))
                    }
                    let value = try extract(UnsafeRawPointer(storage))
                    storage.initializeMemory(as: UInt8.self, repeating: 0xa5, count: size)
                    storage.deallocate()
                    let copied = try #require(value)
                    #expect(copied.rangeUnresolved == (size > 64))
                    #expect(copied.location == (form == .none ? nil : form))
                    #expect(copied.fillTimeoutMs == timeout)
                    #expect(copied.includeProperties == properties)
                }
            }
        }
    }

    @Test("Incomplete physical prefixes are refused", arguments: [0, 4, 8, 56, 63])
    func shortPrefix(_ size: Int) throws {
        let storage = UnsafeMutableRawPointer.allocate(byteCount: max(size, 4), alignment: 8)
        defer { storage.deallocate() }
        storage.storeBytes(of: UInt32(size), as: UInt32.self)
        #expect(try extract(UnsafeRawPointer(storage)) == nil)
    }

    @Test("A torn or absent event pointer is never dereferenced")
    func shortDetail() throws {
        let offset = MemoryLayout<moq_fetch_request_event_t>.offset(of: \.options)!
        for size in offset..<(offset + MemoryLayout<UnsafeRawPointer>.size) {
            #expect(try extract(UnsafeRawPointer(bitPattern: 1), detail: size) == nil)
        }
        #expect(try extract(nil) == nil)
    }

    @Test("Nested location needs its complete v0 prefix", arguments: [0, 4, 47, 48, 64])
    func nestedLocation(_ size: Int) throws {
        var options = moq_fetch_request_options_t()
        options.struct_size = UInt32(MemoryLayout<moq_fetch_request_options_t>.size)
        options.flags = UInt32(MOQ_FETCH_REQUEST_INCLUDE_PROPERTIES)
        options.location.struct_size = UInt32(size)
        let value = try withUnsafePointer(to: &options) { try extract(UnsafeRawPointer($0)) }
        #expect((value != nil) == (size >= MOQ_LOCATION_FILTER_CFG_V0_SIZE))
        if let value {
            #expect(!value.rangeUnresolved && value.location == nil && value.fillTimeoutMs == nil)
            #expect(value.includeProperties)
        }
    }

    @Test("Existing three-argument construction keeps its defaults")
    func construction() {
        let value = FetchRequestOptions(rangeUnresolved: true, location: nil, fillTimeoutMs: 99)
        #expect(value.rangeUnresolved && value.location == nil && value.fillTimeoutMs == 99)
        #expect(value.includeProperties)
    }

    @Test("The limit convenience name keeps the unknown enum case")
    func limitAlias() {
        #expect(MOQ_ERR_LIMIT == -16)
        #expect(MoQError.limit == .unknown(MOQ_ERR_LIMIT))
    }
}
