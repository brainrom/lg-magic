import pyqtgraph.opengl as gl
import numpy as np
from pyqtgraph.Qt import QtWidgets
import math

app = QtWidgets.QApplication([])

view = gl.GLViewWidget()
view.show()
view.setCameraPosition(distance=5)

# Make a box (cube)
verts = np.array([
    [1,1,1], [-1,1,1], [-1,-1,1], [1,-1,1],
    [1,1,-1], [-1,1,-1], [-1,-1,-1], [1,-1,-1]
])
faces = np.array([
    [0,1,2], [0,2,3],
    [4,5,6], [4,6,7],
    [0,1,5], [0,5,4],
    [2,3,7], [2,7,6],
    [1,2,6], [1,6,5],
    [0,3,7], [0,7,4]
])
colors = np.ones((faces.shape[0], 4))
colors[:,0] = np.linspace(0,1,faces.shape[0])  # color variation

current_quat = []
mesh = None

def quat_to_axis_angle(quat):
    w, x, y, z = quat
    if w > 1:  # normalize
        quat = quat / np.linalg.norm(quat)
        w, x, y, z = quat

    angle = 2 * math.acos(w)  # radians
    s = math.sqrt(1 - w*w)
    if s < 1e-8:  # avoid division by zero
        axis = np.array([1, 0, 0])  # arbitrary
    else:
        axis = np.array([x, y, z]) / s
    return np.degrees(angle), axis

def update(quat):
    global mesh
    if mesh is None:
        mesh = gl.GLMeshItem(vertexes=verts, faces=faces, faceColors=colors, smooth=False, drawEdges=False)
        view.addItem(mesh)

    angle, axis = quat_to_axis_angle(quat)
    mesh.resetTransform()  # clear previous rotations
    mesh.rotate(angle, axis[0], axis[1], axis[2])

def start():
    app.exec()
