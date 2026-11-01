// Combine-Protokolle, die Foundation (JSONEncoder/-Decoder, PropertyListEncoder/-Decoder) für Combine bereitstellt (ClokeOS: nur die
// Protokolle, kein Publisher). Apples Combine ist nicht offen.
public protocol TopLevelEncoder {
  associatedtype Output
  func encode<T: Encodable>(_ value: T) throws -> Output
}
public protocol TopLevelDecoder {
  associatedtype Input
  func decode<T: Decodable>(_ type: T.Type, from: Input) throws -> T
}
