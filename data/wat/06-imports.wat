(module
  (import "trek" "kirk" (func $kirk))
  (import "trek" "picard" (func $picard (param i32) (result i64)))
  (import "trek" "sisko" (func $sisko (param i64) (result f32)))
  (import "trek" "janeway" (func $janeway (param f32) (result f64)))
  (import "trek" "archer" (func $archer (param f64)))
)
