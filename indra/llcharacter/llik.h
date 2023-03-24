/**
 * @file llik.h
 * @brief Implementation of LLIK::Solver class and related helpers.
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2021, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */
#ifndef LL_LLIK_H
#define LL_LLIK_H

#include <unordered_map>
#include <set>
#include <string>
#include <vector>
#include <functional>

#include "llmath.h"
#include "llquaternion.h"
#include "m4math.h"
#include "v3math.h"
#include "v4math.h"
#include "xform.h"

#include "llsingleton.h"

class LLJoint;

//#define DEBUG_LLIK_UNIT_TESTS

namespace LLIK
{
// Inverse Kinematics (IK) for humanoid character.
//
// The Solver uses Forward and Backward Reaching Inverse Kinematics (FABRIK)
// algorithm to iterate toward a solution:
//
//      http://andreasaristidou.com/FABRIK.html
//
// The Joints can have Constraints which limit their parent-local orientations.
//

class Solver;
class Joint;

// config flags
constexpr U8 CONFIG_FLAG_LOCAL_POS = 1 << 0;
constexpr U8 CONFIG_FLAG_LOCAL_ROT = 1 << 1;
constexpr U8 CONFIG_FLAG_LOCAL_SCALE = 1 << 2;
constexpr U8 CONFIG_FLAG_DISABLE_CONSTRAINT = 1 << 3;
constexpr U8 CONFIG_FLAG_TARGET_POS = 1 << 4;
constexpr U8 CONFIG_FLAG_TARGET_ROT = 1 << 5;
constexpr U8 CONFIG_FLAG_HAS_DELEGATED = 1 << 6; // EXPERIMENTAL

constexpr U8 IK_FLAG_LOCAL_ROT = 1 << 1; // IK has adjusted local_rot
constexpr U8 IK_FLAG_ACTIVE = 1 << 5;
constexpr U8 IK_FLAG_LOCAL_ROT_LOCKED = 1 << 7; // local_rot is locked during IK

constexpr U8 MASK_POS = CONFIG_FLAG_TARGET_POS | CONFIG_FLAG_LOCAL_POS;
constexpr U8 MASK_ROT = CONFIG_FLAG_TARGET_ROT | CONFIG_FLAG_LOCAL_ROT;
constexpr U8 MASK_TRANSFORM = MASK_POS | MASK_ROT;
constexpr U8 MASK_LOCAL = CONFIG_FLAG_LOCAL_POS | CONFIG_FLAG_LOCAL_ROT | CONFIG_FLAG_DISABLE_CONSTRAINT;
constexpr U8 MASK_TARGET = CONFIG_FLAG_TARGET_POS | CONFIG_FLAG_TARGET_ROT;

// this mask relates to LLJointState::Usage enum
constexpr U8 MASK_JOINT_STATE_USAGE = CONFIG_FLAG_LOCAL_POS | CONFIG_FLAG_LOCAL_ROT | CONFIG_FLAG_LOCAL_SCALE;

constexpr F32 IK_DEFAULT_ACCEPTABLE_ERROR = 5.0e-4f; // half millimeter

// A Constraint exists at the tip of Joint
// and limits the range of Joint.mLocalRot.
class Constraint
{
public:
    using ptr_t = std::shared_ptr<Constraint>;

    enum ConstraintType
    {
        NULL_CONSTRAINT,
        UNKNOWN_CONSTRAINT,
        SIMPLE_CONE_CONSTRAINT,
        TWIST_LIMITED_CONE_CONSTRAINT,
        ELBOW_CONSTRAINT,
        KNEE_CONSTRAINT,
        ACUTE_ELLIPSOIDAL_CONE_CONSTRAINT,
        DOUBLE_LIMITED_HINGE_CONSTRAINT
    };

    Constraint() {}
    Constraint(ConstraintType type, LLSD &parameters) :
        mType(type)
    {
        mForward = LLVector3(parameters["forward_axis"]);
        mForward.normalize();
    }

    virtual ~Constraint() {};

    virtual LLSD            asLLSD() const;
    virtual size_t          generateHash() const;

    ConstraintType          getType() const { return mType; }
    bool                    enforce(Joint& joint) const;
    virtual LLQuaternion    computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const = 0;
    virtual LLQuaternion    minimizeTwist(const LLQuaternion& joint_local_rot) const;

    // all Constraints have a forward axis
    const LLVector3&        getForwardAxis() const { return mForward; }
    virtual bool            allowsTwist() const { return true; }

    std::string             typeToName() const;

#ifdef DEBUG_LLIK_UNIT_TESTS
    virtual void dumpConfig() const = 0;
#endif
protected:
    LLVector3               mForward { LLVector3::zero };
    ConstraintType          mType{NULL_CONSTRAINT};
};

// SimpleCone Constraint can twist arbitrarily about its 'forward' axis
// but has a uniform bend limit for orientations perpendicular to 'forward'.
class SimpleCone : public Constraint
{
    // Constrains forward axis inside cone.
    //
    //        / max_angle
    //       /
    //   ---@--------> forward
    //       \
    //        \ max_angle
    //
public:
    SimpleCone(const LLVector3& forward_axis, F32 max_angle);
    SimpleCone(LLSD &parameters);

    ~SimpleCone() {}

    LLSD    asLLSD() const override;
    size_t  generateHash() const override;


    LLQuaternion computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const override;
#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const override;
#endif
private:
    F32 mMaxAngle;

    F32 mCosConeAngle;
    F32 mSinConeAngle;
};

// TwistLimitedCone Constraint can has limited twist about its 'forward' axis
// but has a uniform bend limit for orientations perpendicular to 'forward'.
class TwistLimitedCone : public Constraint
{
    // A constraint for the shoulder.  Like SimpleCone but with limited twist
    //
    // View from side:                 View with foward out of page:
    //                                         max_twist
    //        / cone_angle                  | /
    //       /                              |/
    //   ---@--------> forward_axis    ----(o)----> perp_axis
    //       \                             /|
    //        \ cone_angle                / |
    //                             min_twist
    //
public:
    TwistLimitedCone(const LLVector3& forward_axis, F32 cone_angle, F32 min_twist, F32 max_twist);
    TwistLimitedCone(LLSD &parameters);
    ~TwistLimitedCone(){}

    LLSD        asLLSD() const override;
    size_t      generateHash() const override;

    LLQuaternion computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const override;
    LLQuaternion minimizeTwist(const LLQuaternion& joint_local_rot) const override;
#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const override;
#endif
private:
    F32 mConeAngle;

    F32 mCosConeAngle;
    F32 mSinConeAngle;
    F32 mMinTwist;
    F32 mMaxTwist;
};

// ElbowConstraint can only bend (with limits) about its 'pivot' axis
// and allows limited twist about its 'forward' axis.
class ElbowConstraint : public Constraint
{
    // A Constraint for Elbow: limited hinge with limited twist about forward (forearm) axis.
    //
    // View from the side,             View with foreward axis out of page:
    // with pivot axis out of page:
    //                                      up  max_twist
    //        / max_bend                     | /
    //       /                               |/
    //  ---(o)--------+  forward        ----(o)----> left
    //       \                              /|
    //        \ min_bend                   / |
    //                              min_twist
public:
    ElbowConstraint(const LLVector3& forward_axis, const LLVector3& pivot_axis, F32 min_bend, F32 max_bend, F32 min_twist, F32 max_twist);
    ElbowConstraint(LLSD &parameters);

    LLSD asLLSD() const override;
    size_t generateHash() const override;

    LLQuaternion computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const override;
    LLQuaternion minimizeTwist(const LLQuaternion& joint_local_rot) const override;
#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const override;
#endif
private:
    LLVector3 mPivotAxis;
    LLVector3 mLeft;
    F32 mMinBend;
    F32 mMaxBend;
    F32 mMinTwist;
    F32 mMaxTwist;
};

// KneeConstraint only allows bend (limited) about its 'pivot' axis
// but does not allow any twist about its 'forward' axis.
class KneeConstraint : public Constraint
{
    // A Constraint for knee, or finger. Like ElbowConstraint but
    // no twist allowed, min/max limits on angle about pivot.
    //
    // View from the side, with pivot axis out of page:
    //
    //        / max_bend
    //       /
    //  ---(o)--------+
    //       \
    //        \ min_bend
    //
public:
    KneeConstraint(const LLVector3& forward_axis, const LLVector3& pivot_axis, F32 min_bend, F32 max_bend);
    KneeConstraint(LLSD &parameters);

    LLSD asLLSD() const override;
    size_t generateHash() const override;

    LLQuaternion computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const override;
    bool allowsTwist() const override { return false; }
    LLQuaternion minimizeTwist(const LLQuaternion& joint_local_rot) const override;
#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const override;
#endif
private:
    LLVector3 mPivotAxis;
    LLVector3 mLeft;
    F32 mMinBend;
    F32 mMaxBend;
};

// AcuteEllipsoidalCone is like SimpleCone but with asymmetric radiuses in
// the up, left, down, right directions.  In other words: it has non-symmetric
// bend limits for axes perpendicular to its 'forward' axis.  It was implemented
// mostly as an exercise, since it is similar to the Constraint described in
// the original FABRIK paper. The geometry of the ellipsoidal boundary are
// described by defining the forward offset of the "cross" of radiuses.  Each
// quadrant of the cross in the left-up plane is bound by an elliptical curve that
// depends on its bounding radiuses.
//
//     up  left            |
//      | /                | /
//      |/                 |/
//   ---@------------------+
//           forward      /|
//                         |
//
class AcuteEllipsoidalCone : public Constraint
{
public:
    AcuteEllipsoidalCone(
            const LLVector3& forward_axis,
            const LLVector3& up_axis,
            F32 forward,
            F32 up,
            F32 left,
            F32 down,
            F32 right);
    AcuteEllipsoidalCone(LLSD &parameters);

    LLSD    asLLSD() const override;
    size_t  generateHash() const override;

    LLQuaternion computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const override;
#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const override;
#endif
private:
    LLVector3 mUp;
    LLVector3 mLeft;

    F32 mXForward;
    F32 mXUp;
    F32 mXDown;
    F32 mXLeft;
    F32 mXRight;

    // for each quadrant we cache these parameters to help
    // us project onto each partial ellipse.
    F32 mQuadrantScales[4];
    F32 mQuadrantCosAngles[4];
    F32 mQuadrantCotAngles[4];
};

// The DoubleLimitedHinge constraint is intended for uses on Joints like
// the wrist, or first finger Joints.  It allows for yaw and pitch bends
// but zero twist.
class DoubleLimitedHinge : public Constraint
{
    // A Constraint for first finger bones.
    // No twist allowed, min/max limits on yaw, then pitch.
    //
    // View from above                     View from right
    // with UP out of page                 (remember to use right-hand-rule)
    //
    //   left_axis                            up_axis
    //      |                                   |
    //      | / max_yaw_angle                   | / min_pitch_angle
    //      |/                                  |/
    //  ---(o)--------> forward_axis        ---(x)--------> forward_axis
    //    up \                              left \
    //        \ min_yaw_angle                     \ max_pitch_angle
    //
public:
    DoubleLimitedHinge(
            const LLVector3& forward_axis,
            const LLVector3& up_axis,
            F32 min_yaw,
            F32 max_yaw,
            F32 min_pitch,
            F32 max_pitch);
    DoubleLimitedHinge(LLSD &parameters);

    LLSD    asLLSD() const override;
    size_t  generateHash() const override;

    LLQuaternion computeAdjustedLocalRot(const LLQuaternion& joint_local_rot) const override;
    LLQuaternion minimizeTwist(const LLQuaternion& joint_local_rot) const override;
#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const override;
#endif
private:
    LLVector3 mUp;
    LLVector3 mLeft; // mUp X mForward
    F32 mMinYaw;
    F32 mMaxYaw;
    F32 mMinPitch;
    F32 mMaxPitch;
};

// Joint represents a constrained bone in the skeleton hierarchy.
// It typically has a parent Joint, a fixed mLocalPos position in its
// parent's local-frame, and a fixed mBone to its 'end' position
// in its own local-frame.  A summary of its important data members
// is as follows:
//
//     mLocalPos = tip position in parent's local-frame
//     mLocalRot = orientation of Joint's tip relative to parent's local-frame
//     mBone = invariant end position in local-frame
//     mPos = tip position in world-frame (we call it 'world-frame'
//         but really it is the 'root-frame' of the Skeleton hierarchy).
//     mRot = orientation of Joint in world-frame.
//
// Some important formula to keep in mind:
//
//     mPos = mParent->mPos + mLocalPos * mParent->mRot
//     mRot = mLocalRot * mParent->mRot
//
// The world-frame 'end' position of the Joint can be calculated:
//
//     world_end_pos = mPos + mBone * mRot
//
class Joint
{
public:
    class Config
    {
    public:
        // local info is in parent-frame
        bool hasLocalPos() const { return (mFlags & CONFIG_FLAG_LOCAL_POS) > 0; }
        bool hasLocalRot() const { return (mFlags & CONFIG_FLAG_LOCAL_ROT) > 0; }
        bool hasLocalScale() const { return (mFlags & CONFIG_FLAG_LOCAL_SCALE) > 0; }
        bool constraintIsDisabled() const { return (mFlags & CONFIG_FLAG_DISABLE_CONSTRAINT) > 0; }
        void setLocalPos(const LLVector3& pos);
        void setLocalRot(const LLQuaternion& rot);
        void setLocalScale(const LLVector3& scale);
        void disableConstraint() { mFlags |= CONFIG_FLAG_DISABLE_CONSTRAINT; }
        const LLVector3& getLocalPos() const { return mLocalPos; }
        const LLQuaternion& getLocalRot() const { return mLocalRot; }
        const LLVector3& getLocalScale() const { return mLocalScale; }

        // target info is in skeleton root-frame
        bool hasTargetPos() const { return (mFlags & CONFIG_FLAG_TARGET_POS) > 0; }
        bool hasTargetRot() const { return (mFlags & CONFIG_FLAG_TARGET_ROT) > 0; }
        void setTargetPos(const LLVector3& pos);
        void setTargetRot(const LLQuaternion& rot);
        const LLVector3& getTargetPos() const { return mTargetPos; }
        const LLQuaternion& getTargetRot() const { return mTargetRot; }

        void delegate() { mFlags |= CONFIG_FLAG_HAS_DELEGATED; } // EXPERIMENTAL
        bool hasDelegated() const { return (mFlags & CONFIG_FLAG_HAS_DELEGATED) > 0; } // EXPERIMENTAL

        U8 getFlags() const { return mFlags; }

        void updateFrom(const Config& other_config);

    private:
        LLVector3 mLocalPos;
        LLQuaternion mLocalRot;
        LLVector3 mLocalScale;
        LLVector3 mTargetPos;
        LLQuaternion mTargetRot;
        U8 mFlags = 0; // per-feature bits
    };

    using ptr_t = std::shared_ptr<Joint>;
    using child_vec_t = std::vector<ptr_t>;

    Joint(LLJoint* info);
    void resetFromInfo();

    void addChild(const ptr_t& child);
    void setParent(const ptr_t& parent);
    void resetRecursively();
    void relaxRotationsRecursively(F32 blend_factor);
    F32 recursiveComputeLongestChainLength(F32 length) const;

    void updateGeometry(const LLVector3& local_pos, const LLVector3& bone);

    LLVector3 computeEndTargetPos() const;
    LLVector3 computeWorldTipOffset() const;
    void updateEndInward();
    void updateEndOutward();
    void updateBranchRoot();
    void updateInward(const ptr_t& child);
    void updatePosAndRotFromParent();
    void updateOutward();
    void applyLocalRot();
    void updateLocalRot();
    LLQuaternion computeParentRot() const;
    void updateChildLocalRots() const;

    LLVector3 computePosFromParent() const;
    const LLVector3& getWorldTipPos() const { return mPos; }
    const LLQuaternion& getWorldRot() const { return mRot; }
    LLVector3 computeWorldEndPos() const;
    void setWorldPos(const LLVector3& pos);
    void setLocalPos(const LLVector3& pos);
    void setWorldRot(const LLQuaternion& rot);
    void setLocalRot(const LLQuaternion& new_local_rot);
    void setLocalScale(const LLVector3& scale);
    LLVector3 getPreScaledLocalPos() const;
    void adjustWorldRot(const LLQuaternion& adjustment);
    void shiftPos(const LLVector3& shift);

    void setConfig(const Config& config);

    void setTargetPos(const LLVector3& pos);
    LLVector3 getTargetPos() const { return mConfig->getTargetPos(); }

    const Config* getConfig() const { return mConfig; }
    bool hasPosTarget() const { return (mConfigFlags & CONFIG_FLAG_TARGET_POS) > 0; }
    bool hasRotTarget() const { return (mConfigFlags & CONFIG_FLAG_TARGET_ROT) > 0; }
    U8 getConfigFlags() const { return mConfigFlags; }
    U8 getHarvestFlags() const { return (mConfigFlags | mIkFlags) & MASK_LOCAL; }
    void resetFlags();
    void lockLocalRot(const LLQuaternion& local_rot);

    void setConstraint(std::shared_ptr<Constraint> constraint) { mConstraint = constraint; }
    bool enforceConstraint();
    void updateWorldTransformsRecursively();

    const LLQuaternion& getLocalRot() const { return mLocalRot; }
    S16 getID() const { return mID; }
    ptr_t getSingleActiveChild();
    const LLVector3& getBone() const { return mBone; }
    const LLVector3& getLocalPos() const { return mLocalPos; }
    const LLVector3& getLocalScale() const { return mLocalScale; }
    F32 getBoneLength() const { return mBone.length(); }
    F32 getLocalPosLength() const { return mLocalPosLength; }

    ptr_t getParent() { return mParent; }
    void activate() { mIkFlags |= IK_FLAG_ACTIVE; }
    bool isActive() const { return mIkFlags & IK_FLAG_ACTIVE; }
    bool hasDisabledConstraint() const { return (mConfigFlags & CONFIG_FLAG_DISABLE_CONSTRAINT) > 0; }

    // Joint::mLocalRot is considered "locked" when its mConfigFlag's FLAG_LOCAL_ROT bit is set
    bool localRotLocked() const { return (mIkFlags & IK_FLAG_LOCAL_ROT_LOCKED) > 0; }

    size_t getNumChildren() const { return mChildren.size(); }

    void transformTargetsToParentLocal(std::vector<LLVector3>& local_targets) const;
    bool swingTowardTargets(const std::vector<LLVector3>& local_targets, const std::vector<LLVector3>& world_targets);
    void twistTowardTargets(const std::vector<LLVector3>& local_targets, const std::vector<LLVector3>& world_targets);
    void untwist();

    // We call flagForHarvest() when we expect the joint to be updated by IK
    // so we know to harvest its mLocalRot later.
    void flagForHarvest() { mIkFlags |= IK_FLAG_LOCAL_ROT; }

#ifdef DEBUG_LLIK_UNIT_TESTS
    void dumpConfig() const;
    void dumpState() const;
#endif

    void collectTargetPositions(std::vector<LLVector3>& local_targets, std::vector<LLVector3>& world_targets) const;

protected:
    void reset();
    void relaxRot(F32 blend_factor);

protected:
    std::vector<ptr_t> mChildren;      //List of joint_ids attached to this one.

    LLVector3 mLocalPos; // current pos in parent-frame
    LLVector3 mPos; // pos in world-frame
    // The fundamental position formula is:
    //     mPos = mParent->mPos + mLocalPos * mParent->mRot;

    // Note: there is no mDefaultLocalRot because it is understood to be identity
    LLQuaternion mLocalRot; // orientations in parent-frame
    LLQuaternion mRot;
    // The fundamental orientations formula is:
    //     mRot = mLocalRot * mParent->mRot

    LLVector3 mLocalScale;

    LLVector3 mBone;
    // There is another fundamental formula:
    //    world_end_pos = mPos + mBone * mRot

    ptr_t mParent;
    Constraint::ptr_t mConstraint;
    F32 mLocalPosLength = 0.0f; // cached copy of mLocalPos.length()
    S16 mID = -1;

    // From LLIK::Joint's prespective the LLJoint stores the default
    // non-animated geometry of the Joint, which occasionally needs to
    // be known during IK calculations.  LLJoint was implemented way back,
    // when we were adding raw pointers and we haven't rewritten that logic
    // yet, so to make its raw-pointer nature more clear we call it mInfoPtr.
    const LLJoint* mInfoPtr;

    const Config* mConfig = nullptr; // pointer into Solver::mJointConfigs

    U8 mConfigFlags = 0; // cache of mConfig->mFlags
    U8 mIkFlags = 0; // flags for IK calculations
};


// The Solver maintains a skeleton of connected Joints and computes the
// parent-relative orientations to allow end-effectors to reach their targets.
//
class Solver
{
public:
    using joint_map_t = std::map<S16, Joint::ptr_t>;
    using S16_vec_t = std::vector<S16>;
    using joint_config_map_t = std::map<S16, Joint::Config>;
    using joint_list_t = std::vector<Joint::ptr_t>;
    using chain_map_t = std::map<S16, joint_list_t>;

public:
    Solver();

    // Put skeleton back into default orientation
    // (e.g. T-Pose for humanoid character)
    void resetSkeleton();

    // compute the offset from the "tip" of from_id to the "end" of to_id
    // or the negative when from_id > to_id
    LLVector3 computeReach(S16 to_id, S16 from_id) const;

    // Add a Joint to the skeleton.
    void addJoint(
            S16 joint_id,
            S16 parent_id,
            LLJoint* info_ptr,
            const Constraint::ptr_t& constraint);

    // apply configs and return 'true' if something changed
    bool updateJointConfigs(const joint_config_map_t& configs);

    // Solve the IK problem for the given list of Joint Configurations
    // return max_error of result
    F32 solve();

    // Specify a joint as a 'wrist'.  Will be used to help 'drop the elbow'
    // of the arm to achieve a more realistic solution.
    void addWristID(S16 wrist_id);

    void setRootID(S16 root_id) { mRootID = root_id; }
    S16 getRootID() const { return mRootID; }

    const joint_list_t getActiveJoints() const { return mActiveJoints; }

    // Specify list of joint ids that should be considered as sub-bases
    // e.g. joints that are known to have multipe child chains,
    // like the chest (chains on left and right collar children) or wrists
    // (chain for each fingers).
    void setSubBaseIds(const std::set<S16>& ids);

    // Set list of joint ids that should be considered sub-roots where
    // the IK chains stop.  This HACK was used to remove the spine from the
    // solver before spine constraints were working.
    void setSubRootIds(const std::set<S16>& ids);

    // per-Joint property accessors from outside Solver
    LLQuaternion getJointLocalRot(S16 joint_id) const;
    LLVector3 getJointLocalPos(S16 joint_id) const;
    bool getJointLocalTransform(S16 joint_id, LLVector3& pos, LLQuaternion& rot) const;
    LLVector3 getJointWorldTipPos(S16 joint_id) const;
    LLVector3 getJointWorldEndPos(S16 joint_id) const;
    LLQuaternion getJointWorldRot(S16 joint_id) const;

    void resetJointGeometry(S16 joint_id, const Constraint::ptr_t& constraint);

    void enableDebugIfPossible(); // will be possible when DEBUG_LLIK_UNIT_TESTS defined

#ifdef DEBUG_LLIK_UNIT_TESTS
    size_t getNumJoints() const { return mSkeleton.size(); }
    void dumpConfig() const;
    void dumpActiveState() const;
    F32 getMaxError() const { return mLastError; }
    void updateBounds(const LLVector3& point);
#endif

    void setAcceptableError(F32 slop) { mAcceptableError = slop; }

private:
    bool isSubBase(S16 joint_id) const;
    bool isSubRoot(S16 joint_id) const;
    //void adjustTargets(joint_config_map_t& targets); // EXPERIMENTAL: keep this
    void dropElbow(const Joint::ptr_t& wrist_joint);
    void rebuildAllChains();
    void buildChain(Joint::ptr_t joint, joint_list_t& chain, std::set<S16>& sub_bases);
    void executeFabrikInward(const joint_list_t& chain);
    void executeFabrikOutward(const joint_list_t& chain);
    void shiftChainToBase(const joint_list_t& chain);
    void executeFabrikPass();
    void enforceConstraintsOutward();
    void executeCcdPass(); // EXPERIMENTAL
    void executeCcdInward(const joint_list_t& chain);
    void untwistChain(const joint_list_t& chain);
    F32 measureMaxError();

private:
    joint_map_t mSkeleton;
    joint_config_map_t mJointConfigs;

    chain_map_t mChainMap;
    std::set<S16> mSubBaseIds; // HACK: whitelist of sub-bases
    std::set<S16> mSubRootIds; // HACK: whitelist of sub-roots
    std::set<Joint::ptr_t> mActiveRoots;
    std::vector<Joint::ptr_t> mActiveJoints; // Joints with non-default local-pos
    joint_list_t mWristJoints;
    F32 mAcceptableError = IK_DEFAULT_ACCEPTABLE_ERROR;
    F32 mLastError = 0.0f;
    S16 mRootID;                //ID number of the root joint for this skeleton
#ifdef DEBUG_LLIK_UNIT_TESTS
    LLVector3 mMinPos;
    LLVector3 mMaxPos;
    bool mDebugEnabled = false;
#endif
};

} // namespace LLIK

// Constraint's are 'stateless' configurations so we use a Factory pattern to
// allocate them, which allows multiple Joints with identical constraint configs
// to use a single Constraint instance.
//
class LLIKConstraintFactory: public LLSingleton<LLIKConstraintFactory>
{
    LLSINGLETON_EMPTY_CTOR_C11(LLIKConstraintFactory)

public:
    size_t                          getNumConstraints() const { return mConstraints.size(); } // for unit-test

    LLIK::Constraint::ptr_t         getConstrForJoint(const std::string &joint_name) const;

protected:
    void                            initSingleton() override;

private:
    using constraint_cache_t = std::unordered_map<size_t, LLIK::Constraint::ptr_t>; // turn this into an unordered set?
    using constraint_map_t = std::unordered_map<std::string, LLIK::Constraint::ptr_t>;

    void                            processConstraintMappings(LLSD mappings);
    LLIK::Constraint::ptr_t         getConstraint(LLSD constraint_def);

    static LLIK::Constraint::ptr_t  create(LLSD &data);

    constraint_cache_t  mConstraints;
    constraint_map_t    mJointMapping;
};


namespace std
{   // Specialization for std::hash<Constraint>
    template<>
    struct hash<LLIK::Constraint>
    {
        size_t operator()(LLIK::Constraint const& s) const noexcept
        {
            return s.generateHash();
        }
    };
}

#endif // LL_LLIK_H
