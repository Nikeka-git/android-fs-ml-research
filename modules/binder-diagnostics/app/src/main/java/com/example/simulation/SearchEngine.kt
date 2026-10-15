package com.example.simulation

/** Generates offsets closest to zero first; equal distances prefer the negative offset. */
class SearchEngine {
    fun offsets(searchRange: Int, step: Int): Sequence<Long> {
        require(searchRange >= 0) { "searchRange must be non-negative" }
        require(step > 0) { "step must be positive" }
        return sequence {
            yield(0L)
            var distance = step.toLong()
            while (distance <= searchRange.toLong()) {
                yield(-distance)
                yield(distance)
                distance += step.toLong()
            }
        }
    }
}
