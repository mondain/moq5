import Testing
import CMoQCore
@testable import MoQ

@Suite("Extended ACK observations")
struct ExtendedObservationTests {
    enum Kind: CaseIterable {
        case subscriptionUpdate, publicationUpdate, publishRequest, publishOk

        var expiryEnds: (presence: UInt32, value: UInt32) {
            switch self {
            case .subscriptionUpdate:
                return (end(\moq_subscription_update_ok_event_t.has_expires),
                        end(\moq_subscription_update_ok_event_t.expires_ms))
            case .publicationUpdate:
                return (end(\moq_publication_update_ok_event_t.has_expires),
                        end(\moq_publication_update_ok_event_t.expires_ms))
            case .publishRequest:
                return (end(\moq_publish_request_event_t.has_expires),
                        end(\moq_publish_request_event_t.expires_ms))
            case .publishOk:
                return (end(\moq_publish_ok_event_t.has_expires),
                        end(\moq_publish_ok_event_t.expires_ms))
            }
        }
    }

    private static func end<T, F>(_ field: KeyPath<T, F>) -> UInt32 {
        UInt32(MemoryLayout<T>.offset(of: field)! + MemoryLayout<F>.size)
    }

    private func event(
        _ kind: Kind, largest: (UInt64, UInt64)? = nil,
        expires: UInt64? = nil, size: UInt32? = nil
    ) -> moq_event_t {
        var event = moq_event_t()
        switch kind {
        case .subscriptionUpdate:
            event.kind = MOQ_EVENT_SUBSCRIPTION_UPDATE_OK
            event.detail_size = UInt32(MemoryLayout<moq_subscription_update_ok_event_t>.size)
            event.u.subscription_update_ok.sub._opaque = 27
            event.u.subscription_update_ok.has_largest = largest != nil
            event.u.subscription_update_ok.largest_group = largest?.0 ?? 81
            event.u.subscription_update_ok.largest_object = largest?.1 ?? 82
            event.u.subscription_update_ok.has_expires = expires != nil
            event.u.subscription_update_ok.expires_ms = expires ?? 83
        case .publicationUpdate:
            event.kind = MOQ_EVENT_PUBLICATION_UPDATE_OK
            event.detail_size = UInt32(MemoryLayout<moq_publication_update_ok_event_t>.size)
            event.u.publication_update_ok.pub._opaque = 27
            event.u.publication_update_ok.has_largest = largest != nil
            event.u.publication_update_ok.largest_group = largest?.0 ?? 81
            event.u.publication_update_ok.largest_object = largest?.1 ?? 82
            event.u.publication_update_ok.has_expires = expires != nil
            event.u.publication_update_ok.expires_ms = expires ?? 83
        case .publishRequest:
            event.kind = MOQ_EVENT_PUBLISH_REQUEST
            event.detail_size = UInt32(MemoryLayout<moq_publish_request_event_t>.size)
            event.u.publish_request.pub._opaque = 27
            event.u.publish_request.has_expires = expires != nil
            event.u.publish_request.expires_ms = expires ?? 83
        case .publishOk:
            event.kind = MOQ_EVENT_PUBLISH_OK
            event.detail_size = UInt32(MemoryLayout<moq_publish_ok_event_t>.size)
            event.u.publish_ok.pub._opaque = 27
            event.u.publish_ok.has_expires = expires != nil
            event.u.publish_ok.expires_ms = expires ?? 83
        }
        if let size { event.detail_size = size }
        return event
    }

    private func converted(
        _ kind: Kind, largest: (UInt64, UInt64)? = nil,
        expires: UInt64? = nil, size: UInt32? = nil
    ) -> ExtendedEvent {
        var native = event(kind, largest: largest, expires: expires, size: size)
        return ExtendedEvent.from(&native)
    }

    @Test("Expiry absence, zero and MAX are distinct", arguments: Kind.allCases)
    func expiryValues(_ kind: Kind) {
        let absent = converted(kind)
        let zero = converted(kind, expires: 0)
        let maximum = converted(kind, expires: .max)
        #expect(absent != zero)
        #expect(absent != maximum)
        #expect(zero != maximum)
    }

    @Test("Both Largest components and their presence survive", arguments: [Kind.subscriptionUpdate, .publicationUpdate])
    func largestValues(_ kind: Kind) {
        let locations: [(UInt64, UInt64)?] = [nil, (0, 0), (.max, 0), (0, .max), (.max, .max)]
        let observations = locations.map { converted(kind, largest: $0) }
        for (index, value) in observations.enumerated() {
            for other in observations.dropFirst(index + 1) {
                #expect(value != other)
            }
        }
        #expect(converted(kind, largest: (0, 0), expires: 0) != observations[1])
    }

    @Test("Expiry requires a complete presence/value pair", arguments: Kind.allCases)
    func expiryPrefixes(_ kind: Kind) {
        let ends = kind.expiryEnds
        let absent = converted(kind)
        for size in [ends.presence - 1, ends.presence, ends.value - 1] {
            #expect(converted(kind, expires: .max, size: size) == absent)
        }
        let complete = converted(kind, expires: .max, size: ends.value)
        #expect(complete == converted(kind, expires: .max))
        #expect(complete != absent)
    }

    @Test("Largest requires the flag and both whole uint64 fields", arguments: [Kind.subscriptionUpdate, .publicationUpdate])
    func largestPrefixes(_ kind: Kind) {
        let ends: (presence: UInt32, group: UInt32, object: UInt32)
        switch kind {
        case .subscriptionUpdate:
            ends = (Self.end(\moq_subscription_update_ok_event_t.has_largest),
                    Self.end(\moq_subscription_update_ok_event_t.largest_group),
                    Self.end(\moq_subscription_update_ok_event_t.largest_object))
        case .publicationUpdate:
            ends = (Self.end(\moq_publication_update_ok_event_t.has_largest),
                    Self.end(\moq_publication_update_ok_event_t.largest_group),
                    Self.end(\moq_publication_update_ok_event_t.largest_object))
        default:
            Issue.record("Not an UPDATE acknowledgment"); return
        }
        for size in [ends.presence - 1, ends.presence, ends.group - 1, ends.group, ends.object - 1] {
            #expect(converted(kind, largest: (0, .max), expires: .max, size: size) == converted(kind))
        }
        let largestOnly = converted(kind, largest: (0, .max), expires: .max, size: ends.object)
        #expect(largestOnly == converted(kind, largest: (0, .max)))
        #expect(largestOnly != converted(kind))
    }

    @Test("Copied observations retain exact values after native cleanup", arguments: Kind.allCases)
    func copiedValues(_ kind: Kind) {
        let expiries: [UInt64?] = [nil, 0, .max]
        let locations: [(UInt64, UInt64)?] = [nil, (0, 0), (.max, 0), (0, .max), (.max, .max)]
        for expiry in expiries {
            for location in locations {
                var native = event(kind, largest: location, expires: expiry)
                let value = ExtendedEvent.from(&native)
                native = moq_event_t()
                let copiedExpiry: UInt64?
                switch value {
                case .subscriptionUpdateOk(let info):
                    #expect(info.subscription.raw._opaque == 27)
                    #expect(info.largest?.group == location?.0)
                    #expect(info.largest?.object == location?.1)
                    copiedExpiry = info.expiresMs
                    #expect(Set([info, info]).count == 1)
                case .publicationUpdateOk(let info):
                    #expect(info.publication.raw._opaque == 27)
                    #expect(info.largest?.group == location?.0)
                    #expect(info.largest?.object == location?.1)
                    copiedExpiry = info.expiresMs
                    #expect(Set([info, info]).count == 1)
                case .publishRequest(let info):
                    #expect(info.publication.raw._opaque == 27)
                    copiedExpiry = info.expiresMs
                case .publishOk(let info):
                    #expect(info.publication.raw._opaque == 27)
                    copiedExpiry = info.expiresMs
                default:
                    Issue.record("Missing extended observation"); return
                }
                #expect(copiedExpiry == expiry)
            }
        }
    }

    @Test("Legacy events keep their unknown projection", arguments: Kind.allCases)
    func legacyProjection(_ kind: Kind) {
        var native = event(kind, largest: (0, .max), expires: .max)
        let rawKind = native.kind
        #expect(Event.from(&native) == .unknown(rawKind))
    }
}
