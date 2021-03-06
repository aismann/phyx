#ifdef _WIN32
#include "glad/glad.h"
#endif

#include <GLFW/glfw3.h>

#include "World.h"
#include "Configuration.h"

#include "base/WorkQueue.h"

#include "microprofile/microprofile.h"
#include "microprofile/microprofileui.h"
#include "microprofile/microprofiledraw.h"

struct Vertex
{
    Vector2f position;
    unsigned char r, g, b, a;

};

void RenderBox(std::vector<Vertex>& vertices, Coords2f coords, Vector2f size, int r, int g, int b, int a)
{
    Vector2f axisX = coords.xVector * size.x;
    Vector2f axisY = coords.yVector * size.y;

    Vertex v;

    v.r = r;
    v.g = g;
    v.b = b;
    v.a = a;

    v.position = coords.pos - axisX - axisY;
    vertices.push_back(v);

    v.position = coords.pos + axisX - axisY;
    vertices.push_back(v);

    v.position = coords.pos + axisX + axisY;
    vertices.push_back(v);

    v.position = coords.pos - axisX + axisY;
    vertices.push_back(v);
}

float random(float min, float max)
{
    return min + (max - min) * (float(rand()) / float(RAND_MAX));
}

const struct
{
    Configuration::SolveMode mode;
    const char* name;
} kSolveModes[] =
{
   {Configuration::Solve_Scalar, "Scalar"},

#ifdef __SSE2__
   {Configuration::Solve_SSE2, "SSE2"},
#endif

#ifdef __AVX2__
   {Configuration::Solve_AVX2, "AVX2"},
#endif
};

const struct
{
    Configuration::IslandMode mode;
    const char* name;
} kIslandModes[] =
{
   {Configuration::Island_Single, "Single"},
   {Configuration::Island_Multiple, "Multiple"},
   {Configuration::Island_SingleSloppy, "Single Sloppy"},
   {Configuration::Island_MultipleSloppy, "Multiple Sloppy"},
};

const char* resetWorld(World& world, int scene)
{
    MICROPROFILE_SCOPEI("Init", "resetWorld", -1);

    world.bodies.clear();
    world.collider.manifolds.clear();
    world.collider.manifoldMap.clear();
    world.solver.contactJoints.clear();

    RigidBody* groundBody = world.AddBody(Coords2f(Vector2f(0, 0), 0.0f), Vector2f(10000.f, 10.0f));
    groundBody->invInertia = 0.0f;
    groundBody->invMass = 0.0f;

    world.AddBody(Coords2f(Vector2f(-1000, 1500), 0.0f), Vector2f(30.0f, 30.0f));

    switch (scene % 8)
    {
    case 0:
    {
        for (int bodyIndex = 0; bodyIndex < 20000; bodyIndex++)
        {
            Vector2f pos = Vector2f(random(-500.0f, 500.0f), random(50.f, 1000.0f));
            Vector2f size(4.f, 4.f);

            world.AddBody(Coords2f(pos, 0.f), size);
        }

        return "Falling";
    }

    case 1:
    {
        for (int left = -100; left <= 100; left++)
        {
            for (int bodyIndex = 0; bodyIndex < 100; bodyIndex++)
            {
                Vector2f pos = Vector2f(left * 20, 10 + bodyIndex * 10);
                Vector2f size(10, 5);

                world.AddBody(Coords2f(pos, 0.f), size);
            }
        }

        return "Wall";
    }

    case 2:
    {
        for (int step = 0; step < 100; ++step)
        {
            Vector2f pos = Vector2f(0, 1005 - step * 10);
            Vector2f size(10 + step * 5, 5);

            world.AddBody(Coords2f(pos, 0.f), size);
        }

        return "Pyramid";
    }

    case 3:
    {
        for (int step = 0; step < 100; ++step)
        {
            Vector2f pos = Vector2f(0, 15 + step * 10);
            Vector2f size(10 + step * 5, 5);

            world.AddBody(Coords2f(pos, 0.f), size);
        }

        return "Reverse Pyramid";
    }

    case 4:
    {
        for (int left = -100; left <= 100; left++)
        {
            for (int bodyIndex = 0; bodyIndex < 150; bodyIndex++)
            {
                Vector2f pos = Vector2f(left * 15, 15 + bodyIndex * 10);
                Vector2f size(5 - bodyIndex * 0.03f, 5);

                world.AddBody(Coords2f(pos, 0.f), size);
            }
        }

        return "Stacks";
    }

    case 5:
    {
        world.AddBody(Coords2f(Vector2f(0.f, 400.f), 0.f), Vector2f(600.f, 10.f))->invMass = 0.f;
        world.AddBody(Coords2f(Vector2f(800.f, 200.f), 0.f), Vector2f(400.f, 10.f))->invMass = 0.f;

        for (int bodyIndex = 0; bodyIndex < 20000; bodyIndex++)
        {
            Vector2f pos = Vector2f(random(0.0f, 500.0f), random(500.f, 2500.0f));
            Vector2f size(4.f, 4.f);

            world.AddBody(Coords2f(pos, 0.f), size);
        }

        return "Stacks";
    }

    case 6:
    {
        world.AddBody(Coords2f(Vector2f(0.f, 400.f), 0.f), Vector2f(600.f, 10.f))->invMass = 0.f;
        world.AddBody(Coords2f(Vector2f(800.f, 200.f), 0.f), Vector2f(400.f, 10.f))->invMass = 0.f;

        RigidBody* body = world.AddBody(Coords2f(Vector2f(500.f, 500.f), -0.5f), Vector2f(600.f, 10.f));
        body->invMass = 0.f;
        body->invInertia = 0.f;

        for (int bodyIndex = 0; bodyIndex < 10000; bodyIndex++)
        {
            Vector2f pos1 = Vector2f(random(200.0f, 500.0f), random(500.f, 2500.0f));
            Vector2f pos2 = Vector2f(random(-500.0f, -200.0f), random(500.f, 2500.0f));
            Vector2f size(4.f, 4.f);

            world.AddBody(Coords2f(pos1, 0.f), size);
            world.AddBody(Coords2f(pos2, 0.f), size);
        }

        return "Dual Stacks";
    }

    case 7:
    {
        for (int group = -5; group <= 5; ++group)
        {
            RigidBody* splitter = world.AddBody(Coords2f(Vector2f(group * 300, 500.f), 0.f), Vector2f(20.f, 1000.f));
            splitter->invMass = 0.f;
            splitter->invInertia = 0.f;

            for (int bodyIndex = 0; bodyIndex < 4500; bodyIndex++)
            {
                Vector2f pos = Vector2f(group * 300 + random(50.f, 250.0f), random(50.f, 1500.0f));
                Vector2f size(4.f, 4.f);

                world.AddBody(Coords2f(pos, 0.f), size);
            }
        }

        return "Islands";
    }
    }

    return "Empty";
}

bool keyPressed[GLFW_KEY_LAST + 1];
int mouseScrollDelta = 0;

static void errorCallback(int error, const char* description)
{
    fputs(description, stderr);
}

static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    keyPressed[key] = (action == GLFW_PRESS);
}

static void scrollCallback(GLFWwindow* window, double x, double y)
{
    mouseScrollDelta = y;
}

int main(int argc, char** argv)
{
    MicroProfileOnThreadCreate("Main");
    MicroProfileSetEnableAllGroups(true);
    MicroProfileSetForceMetaCounters(true);

    int windowWidth = 1280, windowHeight = 1024;

    std::unique_ptr<WorkQueue> queue(new WorkQueue(WorkQueue::getIdealWorkerCount() - 1));

    World world;

    int currentSolveMode = sizeof(kSolveModes) / sizeof(kSolveModes[0]) - 1;
    int currentIslandMode = sizeof(kIslandModes) / sizeof(kIslandModes[0]) - 1;
    int currentScene = 0;

    const char* currentSceneName = resetWorld(world, currentScene);

    const float gravity = -200.0f;
    const float integrationTime = 1 / 60.f;

    world.gravity = gravity;

    glfwSetErrorCallback(errorCallback);

    if (!glfwInit()) return 1;

    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "PhyX", NULL, NULL);
    if (!window) return 1;

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetKeyCallback(window, keyCallback);
    glfwSetScrollCallback(window, scrollCallback);

#ifdef _WIN32
	if (!gladLoadGL()) return 2;
#endif

    MicroProfileDrawInitGL();
    MicroProfileGpuInitGL();

    bool paused = false;

    double prevUpdateTime = 0.0f;

    std::vector<Vertex> vertices;

    float viewOffsetX = -500;
    float viewOffsetY = -40;
    float viewScale = 0.5f;

    int frameIndex = 0;

    while (!glfwWindowShouldClose(window))
    {
        MicroProfileFlip();

        MICROPROFILE_SCOPEI("MAIN", "Frame", 0xffee00);

        MICROPROFILE_LABELF("MAIN", "Index %d", frameIndex++);

        int width, height;
        glfwGetWindowSize(window, &width, &height);

        int frameWidth, frameHeight;
        glfwGetFramebufferSize(window, &frameWidth, &frameHeight);

        double mouseX, mouseY;
        glfwGetCursorPos(window, &mouseX, &mouseY);

        glViewport(0, 0, frameWidth, frameHeight);
        glClearColor(0.2f, 0.2f, 0.2f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(viewOffsetX / viewScale, width / viewScale + viewOffsetX / viewScale, viewOffsetY / viewScale, height / viewScale + viewOffsetY / viewScale, 1.f, -1.f);

        vertices.clear();

        if (glfwGetTime() > prevUpdateTime + integrationTime)
        {
            prevUpdateTime += integrationTime;

            if (!paused)
            {
                RigidBody* draggedBody = &world.bodies[1];
                Vector2f dragTarget =
                    glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT)
                    ? Vector2f(mouseX + viewOffsetX, height + viewOffsetY - mouseY) / viewScale
                    : draggedBody->coords.pos;

                Vector2f dstVelocity = (dragTarget - draggedBody->coords.pos) * 5e1f;

                draggedBody->acceleration.y -= gravity;
                draggedBody->acceleration += (dstVelocity - draggedBody->velocity) * 5e0;

                Configuration config = { kSolveModes[currentSolveMode].mode, kIslandModes[currentIslandMode].mode, 15, 15 };
                world.Update(*queue, integrationTime, config);
            }
        }

        char stats[256];
        sprintf(stats, "Scene: %s | Bodies: %d Manifolds: %d Contacts: %d Islands: %d (biggest: %d) | Cores: %d; Solve: %s; Island: %s; Iterations: %.2f",
            currentSceneName,
            int(world.bodies.size),
            int(world.collider.manifolds.size),
            int(world.solver.contactJoints.size),
            int(world.solver.islandCount),
            int(world.solver.islandMaxSize),
            int(queue->getWorkerCount() + 1),
            kSolveModes[currentSolveMode].name,
            kIslandModes[currentIslandMode].name,
            0.f);

        {
            MICROPROFILE_SCOPEI("Render", "Render", 0xff0000);

            {
                MICROPROFILE_SCOPEI("Render", "Prepare", -1);

                for (int bodyIndex = 0; bodyIndex < world.bodies.size; bodyIndex++)
                {
                    RigidBody* body = &world.bodies[bodyIndex];
                    Coords2f bodyCoords = body->coords;
                    Vector2f size = body->geom.size;

                    float colorMult = float(bodyIndex) / float(world.bodies.size) * 0.5f + 0.5f;
                    int r = 50 * colorMult;
                    int g = 125 * colorMult;
                    int b = 218 * colorMult;

                    if (bodyIndex == 1) //dragged body
                    {
                        r = 242;
                        g = 236;
                        b = 164;
                    }

                    RenderBox(vertices, bodyCoords, size, r, g, b, 255);
                }

                if (glfwGetKey(window, GLFW_KEY_V))
                {
                    for (int manifoldIndex = 0; manifoldIndex < world.collider.manifolds.size; manifoldIndex++)
                    {
                        Manifold& man = world.collider.manifolds[manifoldIndex];

                        for (int collisionNumber = 0; collisionNumber < man.pointCount; collisionNumber++)
                        {
                            ContactPoint& cp = world.collider.contactPoints[man.pointIndex + collisionNumber];

                            Coords2f coords = Coords2f(Vector2f(0.0f, 0.0f), 3.1415f / 4.0f);

                            coords.pos = world.bodies[man.body1Index].coords.pos + cp.delta1;

                            float redMult = cp.isNewlyCreated ? 0.5f : 1.0f;

                            RenderBox(vertices, coords, Vector2f(3.0f, 3.0f), 100, 100 * redMult, 100 * redMult, 100);

                            coords.pos = world.bodies[man.body2Index].coords.pos + cp.delta2;

                            RenderBox(vertices, coords, Vector2f(3.0f, 3.0f), 150, 150 * redMult, 150 * redMult, 100);
                        }
                    }
                }
            }

            {
                MICROPROFILE_SCOPEI("Render", "Perform", -1);
                MICROPROFILE_SCOPEGPUI("Scene", -1);

                if (!vertices.empty())
                {
                    glEnableClientState(GL_VERTEX_ARRAY);
                    glEnableClientState(GL_COLOR_ARRAY);

                    glVertexPointer(2, GL_FLOAT, sizeof(Vertex), &vertices[0].position);
                    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), &vertices[0].r);

                    glDrawArrays(GL_QUADS, 0, vertices.size());

                    glDisableClientState(GL_VERTEX_ARRAY);
                    glDisableClientState(GL_COLOR_ARRAY);
                }
            }

            {
                MICROPROFILE_SCOPEI("Render", "Profile", -1);
                MICROPROFILE_SCOPEGPUI("Profile", -1);

                MicroProfileBeginDraw(width, height, 1.f);

                MicroProfileDraw(width, height);
                MicroProfileDrawText(2, height - 12, 0xffffffff, stats, strlen(stats));

                MicroProfileEndDraw();
            }
        }

        MICROPROFILE_COUNTER_ADD("frame/count", 1);

        {
            MICROPROFILE_SCOPEI("MAIN", "Flip", 0xffee00);

            glfwSwapBuffers(window);
        }

        {
            MICROPROFILE_SCOPEI("MAIN", "Input", 0xffee00);

            // Handle input
            memset(keyPressed, 0, sizeof(keyPressed));
            mouseScrollDelta = 0;

            glfwPollEvents();

            bool mouseDown0 = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            bool mouseDown1 = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

            MicroProfileMouseButton(mouseDown0, mouseDown1);
            MicroProfileMousePosition(mouseX, mouseY, mouseScrollDelta);
            MicroProfileModKey(glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);

            if (keyPressed[GLFW_KEY_ESCAPE])
                break;

            if (keyPressed[GLFW_KEY_O])
                MicroProfileToggleDisplayMode();

            if (keyPressed[GLFW_KEY_P])
            {
                paused = !paused;
                MicroProfileTogglePause();
            }

            if (keyPressed[GLFW_KEY_I])
                currentIslandMode = (currentIslandMode + 1) % (sizeof(kIslandModes) / sizeof(kIslandModes[0]));

            if (keyPressed[GLFW_KEY_M])
                currentSolveMode = (currentSolveMode + 1) % (sizeof(kSolveModes) / sizeof(kSolveModes[0]));

            if (keyPressed[GLFW_KEY_R])
                currentSceneName = resetWorld(world, currentScene);

            if (keyPressed[GLFW_KEY_S])
                currentSceneName = resetWorld(world, ++currentScene);

            if (keyPressed[GLFW_KEY_C])
            {
                unsigned int cores = queue->getWorkerCount() + 1;
                unsigned int newcores =
                    (cores == WorkQueue::getIdealWorkerCount())
                    ? 1
                    : std::min(cores * 2, WorkQueue::getIdealWorkerCount());

                queue.reset(new WorkQueue(newcores - 1));
            }

            if (glfwGetKey(window, GLFW_KEY_LEFT))
                viewOffsetX -= 10;

            if (glfwGetKey(window, GLFW_KEY_RIGHT))
                viewOffsetX += 10;

            if (glfwGetKey(window, GLFW_KEY_UP))
                viewScale *= 1.05f;

            if (glfwGetKey(window, GLFW_KEY_DOWN))
                viewScale /= 1.05f;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    MicroProfileShutdown();
}