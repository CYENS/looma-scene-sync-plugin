"""Offscreen GPU smoke test. Set LOOMA_SKYBOX_TEST_IMAGE to the conformance PNG."""
import math
import os

import unreal

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
dome = actors.spawn_actor_from_class(unreal.LoomaSkyboxActor, unreal.Vector())
capture_actor = actors.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector())
try:
    mesh = dome.get_component_by_class(unreal.StaticMeshComponent)
    mesh.set_visibility(True)
    texture = unreal.RenderingLibrary.import_file_as_texture2d(world, os.environ["LOOMA_SKYBOX_TEST_IMAGE"])
    texture.set_editor_property("address_x", unreal.TextureAddress.TA_WRAP)
    texture.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
    base_material = unreal.load_asset("/LoomaSceneSync/Materials/M_LoomaPanorama")
    # Commandlets have no regular editor tick to finish asynchronous shaders.
    unreal.MaterialEditingLibrary.get_statistics(base_material)
    material = mesh.create_dynamic_material_instance(0, base_material)
    material.set_texture_parameter_value("Panorama", texture)
    material.set_scalar_parameter_value("Brightness", 1.0)
    material.set_scalar_parameter_value("Yaw", 0.0)
    mesh.set_material(0, material)
    target = unreal.RenderingLibrary.create_render_target2d(world, 512, 512, unreal.TextureRenderTargetFormat.RTF_RGBA16F)
    capture = capture_actor.get_component_by_class(unreal.SceneCaptureComponent2D)
    capture.set_editor_property("texture_target", target)
    capture.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR_NO_ALPHA)
    capture.set_editor_property("primitive_render_mode", unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
    capture.set_editor_property("show_only_actors", [dome])
    capture.set_editor_property("capture_every_frame", False)
    capture.set_editor_property("capture_on_movement", False)

    def sample(pitch, yaw):
        capture_actor.set_actor_rotation(unreal.Rotator(pitch=pitch, yaw=yaw, roll=0), False)
        capture.capture_scene()
        pixel = unreal.RenderingLibrary.read_render_target_raw_pixel(world, target, 256, 256, False)
        unreal.log("UNREAL_SKYBOX_SAMPLE " + str(pixel))
        return pixel

    red = sample(0, 90)  # wire +X = UE +Y
    green = sample(0, 0)  # wire -Z = UE +X
    blue = sample(89.4, 0)
    assert red.r > red.g * 3 and red.r > red.b * 3, red
    assert green.g > green.r * 3 and green.g > green.b * 3, green
    assert blue.b > blue.r * 3 and blue.b > blue.g * 3, blue
    material.set_scalar_parameter_value("Yaw", math.pi / 2)
    turned = sample(0, 0)
    assert abs(turned.r - red.r) < 0.03 and abs(turned.g - red.g) < 0.03, turned

    # Through an orthographic camera the sky is drawn as a perspective camera standing
    # there would see it, at that camera's own fov (asset-demo docs/skyboxes.md), not
    # as one colour. A 90-degree square perspective capture is the reference.
    material.set_scalar_parameter_value("Yaw", 0.0)
    capture_actor.set_actor_rotation(unreal.Rotator(pitch=20, yaw=30, roll=0), False)
    capture.set_editor_property("fov_angle", 90.0)
    points = [(256, 256), (256, 24), (24, 256), (488, 488), (400, 100)]

    def grab():
        capture.capture_scene()
        return [unreal.RenderingLibrary.read_render_target_raw_pixel(world, target, x, y, False) for x, y in points]

    perspective = grab()
    material.set_scalar_parameter_value("TanHalfFov", 1.0)  # tan(90 / 2)
    capture.set_editor_property("projection_type", unreal.CameraProjectionMode.ORTHOGRAPHIC)
    capture.set_editor_property("ortho_width", 1000.0)
    ortho = grab()
    for point, seen, expected in zip(points, ortho, perspective):
        unreal.log("UNREAL_SKYBOX_ORTHO %s %s perspective %s" % (point, seen, expected))
        assert max(abs(seen.r - expected.r), abs(seen.g - expected.g), abs(seen.b - expected.b)) < 0.05, (point, seen, expected)
    spread = max(abs(a.r - b.r) + abs(a.g - b.g) + abs(a.b - b.b) for a in ortho for b in ortho)
    assert spread > 0.1, ("the orthographic sky is one colour", ortho)
    unreal.log("UNREAL_SKYBOX_PASSED")
finally:
    actors.destroy_actor(capture_actor)
    actors.destroy_actor(dome)
