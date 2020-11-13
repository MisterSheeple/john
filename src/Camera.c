#include "Camera.h"
#include "ExtMath.h"
#include "Game.h"
#include "Window.h"
#include "Graphics.h"
#include "Funcs.h"
#include "Gui.h"
#include "Entity.h"
#include "Input.h"
#include "Event.h"
#include "Options.h"
#include "Picking.h"

struct _CameraData Camera;
static struct RayTracer cameraClipPos;
static Vec2 cam_rotOffset;
static cc_bool cam_isForwardThird;
static float cam_deltaX, cam_deltaY;

static void Camera_OnRawMovement(float deltaX, float deltaY) {
	cam_deltaX += deltaX; cam_deltaY += deltaY;
}

/*########################################################################################################################*
*--------------------------------------------------Perspective camera-----------------------------------------------------*
*#########################################################################################################################*/
static void PerspectiveCamera_GetProjection(struct Matrix* proj) {
	float fovy = Game_Fov * MATH_DEG2RAD;
	float aspectRatio = (float)Game.Width / (float)Game.Height;
	Matrix_PerspectiveFieldOfView(proj, fovy, aspectRatio, Gfx.MinZNear, (float)Game_ViewDistance);
}

static void PerspectiveCamera_GetView(struct Matrix* mat) {
	Vec3 pos = Camera.CurrentPos;
	Vec2 rot = Camera.Active->GetOrientation();
	Matrix_LookRot(mat, pos, rot);
	Matrix_MulBy(mat, &Camera.TiltM);
}

static void PerspectiveCamera_GetPickedBlock(struct RayTracer* t) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	Vec3 dir    = Vec3_GetDirVector(p->Yaw * MATH_DEG2RAD, p->Pitch * MATH_DEG2RAD);
	Vec3 eyePos = Entity_GetEyePosition(p);
	float reach = LocalPlayer_Instance.ReachDistance;
	Picking_CalcPickedBlock(&eyePos, &dir, reach, t);
}

#define CAMERA_SENSI_FACTOR (0.0002f / 3.0f * MATH_RAD2DEG)

static Vec2 PerspectiveCamera_GetMouseDelta(double delta) {
	float sensitivity = CAMERA_SENSI_FACTOR * Camera.Sensitivity;
	static float speedX, speedY, newSpeedX, newSpeedY, accelX, accelY;
	Vec2 v;

	if (Camera.Smooth) {
		accelX = (cam_deltaX - speedX) * 35 / Camera.Mass;
		accelY = (cam_deltaY - speedY) * 35 / Camera.Mass;
		newSpeedX = accelX * (float)delta + speedX;
		newSpeedY = accelY * (float)delta + speedY;

		/* High acceleration means velocity overshoots the correct position on low FPS, */
		/* causing wiggling. If newSpeed has opposite sign of speed, set speed to 0 */
		if (newSpeedX * speedX < 0) speedX = 0;
		else speedX = newSpeedX;
		if (newSpeedY * speedY < 0) speedY = 0;
		else speedY = newSpeedY;
	} else {
		speedX = cam_deltaX;
		speedY = cam_deltaY;
	}

	v.X = speedX * sensitivity; v.Y = speedY * sensitivity;
	if (Camera.Invert) v.Y = -v.Y;
	return v;
}

static void PerspectiveCamera_UpdateMouseRotation(double delta) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Entity* e      = &p->Base;

	struct LocationUpdate update;
	float yaw, pitch;
	Vec2 rot = PerspectiveCamera_GetMouseDelta(delta);

	if (Key_IsAltPressed() && Camera.Active->isThirdPerson) {
		cam_rotOffset.X += rot.X; cam_rotOffset.Y += rot.Y;
		return;
	}
	
	yaw   = p->Interp.Next.Yaw   + rot.X;
	pitch = p->Interp.Next.Pitch + rot.Y;
	LocationUpdate_MakeOri(&update, yaw, pitch);

	/* Need to make sure we don't cross the vertical axes, because that gets weird. */
	if (update.Pitch >= 90.0f && update.Pitch <= 270.0f) {
		update.Pitch = p->Interp.Next.Pitch < 180.0f ? 90.0f : 270.0f;
	}
	e->VTABLE->SetLocation(e, &update, false);
}

static void PerspectiveCamera_UpdateMouse(double delta) {
	if (!Gui_GetInputGrab() && WindowInfo.Focused) Window_UpdateRawMouse();

	PerspectiveCamera_UpdateMouseRotation(delta);
	cam_deltaX = 0; cam_deltaY = 0;
}

static void PerspectiveCamera_CalcViewBobbing(float t, float velTiltScale) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Entity* e = &p->Base;

	struct Matrix tiltY, velX;
	float vel;
	if (!Game_ViewBobbing) { Camera.TiltM = Matrix_Identity; return; }

	Matrix_RotateZ(&Camera.TiltM, -p->Tilt.TiltX                  * e->Anim.BobStrength);
	Matrix_RotateX(&tiltY,        Math_AbsF(p->Tilt.TiltY) * 3.0f * e->Anim.BobStrength);
	Matrix_MulBy(&Camera.TiltM, &tiltY);

	Camera.BobbingHor = (e->Anim.BobbingHor * 0.3f) * e->Anim.BobStrength;
	Camera.BobbingVer = (e->Anim.BobbingVer * 0.6f) * e->Anim.BobStrength;

	vel = Math_Lerp(p->OldVelocity.Y + 0.08f, e->Velocity.Y + 0.08f, t);
	Matrix_RotateX(&velX, -vel * 0.05f * p->Tilt.VelTiltStrength / velTiltScale);
	Matrix_MulBy(&Camera.TiltM, &velX);
}


/*########################################################################################################################*
*---------------------------------------------------First person camera---------------------------------------------------*
*#########################################################################################################################*/
static Vec2 FirstPersonCamera_GetOrientation(void) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	Vec2 v;	
	v.X = p->Yaw * MATH_DEG2RAD; v.Y = p->Pitch * MATH_DEG2RAD;
	return v;
}

static Vec3 FirstPersonCamera_GetPosition(float t) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	Vec3 camPos   = Entity_GetEyePosition(p);
	float yaw     = p->Yaw * MATH_DEG2RAD;
	PerspectiveCamera_CalcViewBobbing(t, 1);
	
	camPos.Y += Camera.BobbingVer;
	camPos.X += Camera.BobbingHor * (float)Math_Cos(yaw);
	camPos.Z += Camera.BobbingHor * (float)Math_Sin(yaw);
	return camPos;
}

static cc_bool FirstPersonCamera_Zoom(float amount) { return false; }
static struct Camera cam_FirstPerson = {
	false,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	FirstPersonCamera_GetOrientation, FirstPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, FirstPersonCamera_Zoom,
};


/*########################################################################################################################*
*---------------------------------------------------Third person camera---------------------------------------------------*
*#########################################################################################################################*/
#define DEF_ZOOM 3.0f
static float dist_third = DEF_ZOOM, dist_forward = DEF_ZOOM;

static Vec2 ThirdPersonCamera_GetOrientation(void) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	Vec2 v;	
	v.X = p->Yaw * MATH_DEG2RAD; v.Y = p->Pitch * MATH_DEG2RAD;
	if (cam_isForwardThird) { v.X += MATH_PI; v.Y = -v.Y; }

	v.X += cam_rotOffset.X * MATH_DEG2RAD; 
	v.Y += cam_rotOffset.Y * MATH_DEG2RAD;
	return v;
}

static Vec3 ThirdPersonCamera_GetPosition(float t) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	float dist = cam_isForwardThird ? dist_forward : dist_third;
	Vec3 target, dir;
	Vec2 rot;

	PerspectiveCamera_CalcViewBobbing(t, dist);
	target = Entity_GetEyePosition(p);
	target.Y += Camera.BobbingVer;

	rot = Camera.Active->GetOrientation();
	dir = Vec3_GetDirVector(rot.X, rot.Y);
	Vec3_Negate(&dir, &dir);

	Picking_ClipCameraPos(&target, &dir, dist, &cameraClipPos);
	return cameraClipPos.Intersect;
}

static cc_bool ThirdPersonCamera_Zoom(float amount) {
	float* dist   = cam_isForwardThird ? &dist_forward : &dist_third;
	float newDist = *dist - amount;

	/* Don't allow zooming out when -fly */
	if (newDist > DEF_ZOOM && !LocalPlayer_CheckCanZoom()) newDist = DEF_ZOOM;

	*dist = max(newDist, 2.0f); 
	return true;
}

static struct Camera cam_ThirdPerson = {
	true,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	ThirdPersonCamera_GetOrientation, ThirdPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, ThirdPersonCamera_Zoom,
};
static struct Camera cam_ForwardThird = {
	true,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	ThirdPersonCamera_GetOrientation, ThirdPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, ThirdPersonCamera_Zoom,
};


/*########################################################################################################################*
*-----------------------------------------------------General camera------------------------------------------------------*
*#########################################################################################################################*/
static void OnRawMovement(void* obj, float deltaX, float deltaY) {
	Camera.Active->OnRawMovement(deltaX, deltaY);
}

static void OnHacksChanged(void* obj) {
	struct HacksComp* h = &LocalPlayer_Instance.Hacks;
	/* Leave third person if not allowed anymore */
	if (!h->CanUseThirdPerson || !h->Enabled) Camera_CycleActive();
	/* Check if third person camera is allowed to be scrolled out */
	if (Camera.Active->isThirdPerson) ThirdPersonCamera_Zoom(0);
}

void Camera_CycleActive(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (Game_ClassicMode) return;
	Camera.Active = Camera.Active->next;

	if (!p->Hacks.CanUseThirdPerson || !p->Hacks.Enabled) {
		Camera.Active = &cam_FirstPerson;
	}
	cam_isForwardThird = Camera.Active == &cam_ForwardThird;

	/* reset rotation offset when changing cameras */
	cam_rotOffset.X = 0.0f; cam_rotOffset.Y = 0.0f;
	Camera_UpdateProjection();
}

static struct Camera* cams_head;
static struct Camera* cams_tail;
void Camera_Register(struct Camera* cam) {
	LinkedList_Append(cam, cams_head, cams_tail);
	/* want a circular linked list */
	cam->next = cams_head;
}

static cc_bool cam_focussed;
void Camera_CheckFocus(void) {
	cc_bool focus = Gui_GetInputGrab() == NULL;
	if (focus == cam_focussed) return;
	cam_focussed = focus;

	if (focus) {
		Camera.Active->AcquireFocus();
	} else {
		Camera.Active->LoseFocus();
	}
}

void Camera_UpdateProjection(void) {
	Camera.Active->GetProjection(&Gfx.Projection);
	Gfx_LoadMatrix(MATRIX_PROJECTION, &Gfx.Projection);
	Event_RaiseVoid(&GfxEvents.ProjectionChanged);
}

static void OnInit(void) {
	Camera_Register(&cam_FirstPerson);
	Camera_Register(&cam_ThirdPerson);
	Camera_Register(&cam_ForwardThird);

	Camera.Active = &cam_FirstPerson;
	Event_Register_(&PointerEvents.RawMoved,      NULL, OnRawMovement);
	Event_Register_(&UserEvents.HackPermsChanged, NULL, OnHacksChanged);

#ifdef CC_BUILD_WIN
	Camera.Sensitivity = Options_GetInt(OPT_SENSITIVITY, 1, 200, 40);
#else
	Camera.Sensitivity = Options_GetInt(OPT_SENSITIVITY, 1, 200, 30);
#endif
	Camera.Clipping    = Options_GetBool(OPT_CAMERA_CLIPPING, true);
	Camera.Invert      = Options_GetBool(OPT_INVERT_MOUSE, false);
	Camera.Mass        = Options_GetFloat(OPT_CAMERA_MASS, 1, 100, 20);

	Camera_UpdateProjection();
}

struct IGameComponent Camera_Component = {
	OnInit /* Init  */
};
