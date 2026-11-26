# SPDX-License-Identifier: MIT
"""The orientation maths: normalisation, matrix and Euler angles."""

import math
from collections.abc import Sequence

import pytest

from pico_gui_bno085.orientation import (
    Euler,
    euler_degrees,
    normalised,
    orientation_of,
    rotation_matrix,
)

Axis = tuple[float, float, float]
Quaternion = tuple[float, float, float, float]


def product(left: Quaternion, right: Quaternion) -> Quaternion:
    """Return the Hamilton product of two quaternions, scalar first."""
    lw, lx, ly, lz = left
    rw, rx, ry, rz = right
    return (
        lw * rw - lx * rx - ly * ry - lz * rz,
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
    )


def axis_quaternion(axis: Axis, degrees: float) -> Quaternion:
    """Return the quaternion of a turn of `degrees` about `axis`."""
    half = math.radians(degrees) / 2.0
    sine = math.sin(half)
    return (math.cos(half), axis[0] * sine, axis[1] * sine, axis[2] * sine)


def angles_quaternion(yaw: float, pitch: float, roll: float) -> Quaternion:
    """Return the quaternion of intrinsic Z-Y-X angles, yaw first."""
    turn = product(
        axis_quaternion((0.0, 0.0, 1.0), yaw),
        axis_quaternion((0.0, 1.0, 0.0), pitch),
    )
    return product(turn, axis_quaternion((1.0, 0.0, 0.0), roll))


def angles_of(unit: Quaternion) -> tuple[float, float, float]:
    """Return a quaternion's reading as one comparable tuple."""
    euler = euler_degrees(unit)
    return (euler.yaw_deg, euler.pitch_deg, euler.roll_deg)


def matmul(
    left: Sequence[Sequence[float]],
    right: Sequence[Sequence[float]],
) -> tuple[tuple[float, ...], ...]:
    """Return the matrix product of two three-by-three matrices."""
    return tuple(
        tuple(
            left[row][0] * right[0][column]
            + left[row][1] * right[1][column]
            + left[row][2] * right[2][column]
            for column in range(3)
        )
        for row in range(3)
    )


def assert_rows_close(
    left: Sequence[Sequence[float]],
    right: Sequence[Sequence[float]],
) -> None:
    """Assert two matrices agree row by row, within floating noise."""
    assert len(left) == len(right)
    for row_left, row_right in zip(left, right, strict=True):
        assert row_left == pytest.approx(row_right)


def test_the_identity_is_no_turn() -> None:
    unit = normalised((1.0, 0.0, 0.0, 0.0))
    assert unit == (1.0, 0.0, 0.0, 0.0)
    assert rotation_matrix(unit) == (
        (1.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
    )
    assert euler_degrees(unit) == Euler(0.0, 0.0, 0.0)


def test_a_quaternion_is_normalised_before_use() -> None:
    assert normalised((2.0, 0.0, 0.0, 0.0)) == (1.0, 0.0, 0.0, 0.0)
    assert normalised((0.0, 0.0, 3.0, 4.0)) == (0.0, 0.0, 0.6, 0.8)
    # The four quaternion numbers sit first, whatever else a line carries.
    assert normalised((2.0, 0.0, 0.0, 0.0, 3.14160)) == (1.0, 0.0, 0.0, 0.0)


def test_a_zero_or_nan_quaternion_is_no_orientation() -> None:
    nan = float("nan")
    assert normalised(()) is None
    assert normalised((1.0, 0.0, 0.0)) is None
    assert normalised((0.0, 0.0, 0.0, 0.0)) is None
    assert normalised((nan, nan, nan, nan)) is None
    assert normalised((1.0, 0.0, 0.0, nan)) is None
    assert normalised((float("inf"), 0.0, 0.0, 0.0)) is None
    assert orientation_of(()) is None
    assert orientation_of((0.0, 0.0, 0.0, 0.0, 0.00000)) is None
    assert orientation_of((nan, nan, nan, nan, nan)) is None


def test_a_quaternion_and_its_negative_agree() -> None:
    turn = angles_quaternion(30.0, 20.0, 10.0)
    flipped = (-turn[0], -turn[1], -turn[2], -turn[3])
    assert rotation_matrix(flipped) == rotation_matrix(turn)
    assert angles_of(flipped) == angles_of(turn)


def test_a_quarter_turn_about_each_axis_reads_back() -> None:
    about_x = axis_quaternion((1.0, 0.0, 0.0), 90.0)
    assert_rows_close(
        rotation_matrix(about_x),
        ((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)),
    )
    assert angles_of(about_x) == pytest.approx((0.0, 0.0, 90.0))

    about_y = axis_quaternion((0.0, 1.0, 0.0), 90.0)
    assert angles_of(about_y) == pytest.approx((0.0, 90.0, 0.0))

    about_z = axis_quaternion((0.0, 0.0, 1.0), 90.0)
    assert_rows_close(
        rotation_matrix(about_z),
        ((0.0, -1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)),
    )
    assert angles_of(about_z) == pytest.approx((90.0, 0.0, 0.0))


def test_the_matrix_of_a_product_is_the_product_of_the_matrices() -> None:
    left = angles_quaternion(-120.0, 30.0, 55.0)
    tilt = (0.3, -0.5, math.sqrt(1.0 - 0.3 * 0.3 - 0.5 * 0.5))
    right = axis_quaternion(tilt, 40.0)
    combined = product(left, right)
    assert_rows_close(
        rotation_matrix(combined),
        matmul(rotation_matrix(left), rotation_matrix(right)),
    )


def test_a_composed_rotation_reads_back_its_angles() -> None:
    turn = angles_quaternion(30.0, 20.0, 10.0)
    assert angles_of(turn) == pytest.approx((30.0, 20.0, 10.0))


def test_gimbal_lock_reports_zero_roll_and_the_whole_turn_as_yaw() -> None:
    """At +90° of pitch the observable is yaw minus roll; at -90°, their sum."""
    nose_up = angles_quaternion(30.0, 90.0, 40.0)
    assert angles_of(nose_up) == pytest.approx((-10.0, 90.0, 0.0))
    nose_down = angles_quaternion(30.0, -90.0, 40.0)
    assert angles_of(nose_down) == pytest.approx((70.0, -90.0, 0.0))


def test_the_lock_reading_is_the_same_rotation() -> None:
    nose_up = angles_quaternion(30.0, 90.0, 40.0)
    read_back = angles_quaternion(-10.0, 90.0, 0.0)
    assert_rows_close(rotation_matrix(read_back), rotation_matrix(nose_up))
    nose_down = angles_quaternion(30.0, -90.0, 40.0)
    read_back_down = angles_quaternion(70.0, -90.0, 0.0)
    assert_rows_close(rotation_matrix(read_back_down), rotation_matrix(nose_down))


def test_just_outside_the_lock_the_split_stays_exact() -> None:
    near = angles_quaternion(30.0, 89.9, 40.0)
    assert angles_of(near) == pytest.approx((30.0, 89.9, 40.0))


@pytest.mark.parametrize(
    "yaw",
    [-180.0, -135.0, -90.0, -45.0, 0.0, 45.0, 90.0, 135.0, 180.0],
)
@pytest.mark.parametrize("pitch", [-80.0, -40.0, 0.0, 40.0, 80.0])
@pytest.mark.parametrize(
    "roll",
    [-180.0, -90.0, -45.0, 0.0, 45.0, 90.0, 180.0],
)
def test_angles_round_trip_outside_the_lock_band(
    yaw: float,
    pitch: float,
    roll: float,
) -> None:
    """Angles build a quaternion whose reading is those same angles.

    The grid stays clear of the pitch band around ±90°, where the yaw
    and the roll of one rotation are not two numbers to recover.
    """
    turn = orientation_of(angles_quaternion(yaw, pitch, roll))
    assert turn is not None
    euler = turn.euler
    assert euler.pitch_deg == pytest.approx(pitch)
    for read, wanted in ((euler.yaw_deg, yaw), (euler.roll_deg, roll)):
        if abs(wanted) == 180.0:
            # -180° and 180° name one angle, and the arithmetic lands
            # on either side of the boundary between them.
            assert abs(abs(read) - 180.0) < 1e-9
        else:
            assert read == pytest.approx(wanted)
