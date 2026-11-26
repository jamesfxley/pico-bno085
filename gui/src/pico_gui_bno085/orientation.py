# SPDX-License-Identifier: MIT
"""The maths of a rotation report: unit quaternion, matrix, Euler angles.

The input is the quaternion as LP-6 prints it, the scalar first. This
package reads it as the rotation from the device frame to the report's
reference frame, the reading the board view draws; LP-6 records that
1000-3625 confirms neither the direction of that rotation nor the axes of
the reference frame, so the reading is UNCONFIRMED.
"""

import math
from collections.abc import Sequence
from dataclasses import dataclass

# A rotation whose pitch is this close to either end of its range is read
# with the gimbal-lock convention below rather than the split formulas.
_GIMBAL_EPSILON = 1e-9

Quaternion = tuple[float, float, float, float]

__all__ = (
    "Euler",
    "Orientation",
    "Quaternion",
    "euler_degrees",
    "normalised",
    "orientation_of",
    "rotation_matrix",
)


def normalised(values: Sequence[float]) -> Quaternion | None:
    """Return the unit quaternion of the first four numbers of `values`.

    A sample with fewer than four numbers — an E line, which the bench
    prints in place of a payload line while payload mode is off (LP-4,
    LP-5) — carries no quaternion. A quaternion whose norm is zero or not
    finite is no rotation either, rather than an error, because LP-6
    prints `nan` for a field that is not finite.
    """
    if len(values) < 4:
        return None
    w, x, y, z = values[:4]
    norm = math.sqrt(w * w + x * x + y * y + z * z)
    if not math.isfinite(norm) or norm == 0.0:
        return None
    return (w / norm, x / norm, y / norm, z / norm)


def rotation_matrix(unit: Quaternion) -> tuple[tuple[float, float, float], ...]:
    """Return `unit` as the row-major matrix `ui.scene`'s `rotate_R` takes.

    The matrix turns a column vector, `v' = R v`, so each column is the
    direction one device axis points at in the reference frame, and
    `rotate_R` takes the matrix as the object's own rotation.
    """
    w, x, y, z = unit
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return (
        (1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)),
        (2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)),
        (2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)),
    )


@dataclass(frozen=True)
class Euler:
    """Intrinsic Z-Y-X reading in degrees: yaw, pitch, then roll.

    Yaw turns about Z, then pitch about the new Y, then roll about the
    newest X. Yaw and roll lie in (-180, 180] and pitch in [-90, 90].
    """

    yaw_deg: float
    pitch_deg: float
    roll_deg: float


def _half_turn(radians: float) -> float:
    """Return `radians` in degrees, wrapped into (-180, 180]."""
    degrees = math.degrees(radians)
    if degrees <= -180.0:
        degrees += 360.0
    return degrees


def euler_degrees(unit: Quaternion) -> Euler:
    """Return `unit`'s angles under the convention `Euler` states.

    At either end of the pitch range the yaw and the roll of a rotation
    are one observable rather than two, so roll reports zero and yaw
    takes the whole of it: at +90° the reported yaw is the rotation's
    yaw minus its roll, at -90° their sum. Near the end but outside the
    epsilon band the split stays exact for the matrix while each of the
    two angles moves much for a small change in the matrix.
    """
    matrix = rotation_matrix(unit)
    sin_pitch = -min(1.0, max(-1.0, matrix[2][0]))
    pitch = math.degrees(math.asin(sin_pitch))
    if math.hypot(matrix[2][1], matrix[2][2]) < _GIMBAL_EPSILON:
        sign = 1.0 if sin_pitch > 0.0 else -1.0
        yaw = math.atan2(sign * matrix[1][2], matrix[1][1])
        roll = 0.0
    else:
        yaw = math.atan2(matrix[1][0], matrix[0][0])
        roll = math.atan2(matrix[2][1], matrix[2][2])
    return Euler(_half_turn(yaw), pitch, _half_turn(roll))


@dataclass(frozen=True)
class Orientation:
    """One rotation report as the panel draws it: matrix and angles."""

    matrix: tuple[tuple[float, float, float], ...]
    euler: Euler


def orientation_of(values: Sequence[float]) -> Orientation | None:
    """Return the orientation of a telemetry sample's numbers, or none.

    `values` are the sample's payload numbers (LP-6): the first four are
    the quaternion and any later ones, the heading accuracy or the
    angular velocity, are not orientation. None is a sample that carries
    no rotation.
    """
    unit = normalised(values)
    if unit is None:
        return None
    return Orientation(rotation_matrix(unit), euler_degrees(unit))
