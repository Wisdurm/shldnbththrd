#include "SDL3/SDL_pixels.h"
#include <SDL3/SDL_surface.h>
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_blendmode.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>

#include <SDL3_image/SDL_image.h>

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <cmath>

struct RPoint
{
	std::deque<std::shared_ptr<RPoint>> c;
	SDL_FPoint p;
	RPoint* parent;

	RPoint() : c({}), p({0,0}), parent(nullptr) {};

	RPoint(std::deque<std::shared_ptr<RPoint>> c,
	       SDL_FPoint p, RPoint* parent) : c(c), p(p), parent(parent) {};

	// Copy point and all children
	RPoint(const RPoint &obj)
	{
		// Map old pointers to new ones to resolve cyclical structures
		std::unordered_map<RPoint*, std::shared_ptr<RPoint>> map;

		const auto cp = [&map]
			(this const auto& cp,
			 RPoint* point, const RPoint &target) -> void {

			// Copy old values into new one
			point->c = {};
			point->p = target.p;
			point->parent = nullptr;

			// Copy children
			for (const auto p : target.c) {
				if (map.contains(p.get())) {
					point->c.push_back(map.at(p.get()));
				} else {
					auto neww =
						std::make_shared<RPoint>();
					cp(neww.get(), *p);
					map.insert({p.get(), neww});
					point->c.push_back(neww);
					point->c.back()->parent = point;
				}
			}
		};
		cp(this, obj);
	}
	// Probably not needed?
	RPoint (RPoint&& other) = delete;
};

enum class Mode
{
	SELECT,
	ADD,
	MOVE,
	ROTATE,
};

static std::unordered_map<Mode, std::string> modeNames = {
	{ Mode::SELECT,	"Select"},
	{ Mode::ADD,	"Add"},
	{ Mode::MOVE,	"Move"},
	{ Mode::ROTATE,	"Rotate"},
};

struct Frame {
	std::deque<std::shared_ptr<RPoint>> points;

	Frame(std::deque<std::shared_ptr<RPoint>> points) : points(points) {}
	Frame(const Frame &obj) : points({})
	{
		// Copy points
		for (const auto p : obj.points) {
			points.push_back(std::make_shared<RPoint>(*p));
		}
	}
	// Never move
	Frame (Frame&& other) = delete;
};

void drawLine(const RPoint& p);
void ExportAnimation();
constexpr float Distance(SDL_FPoint p1, SDL_FPoint p2);

// SDL

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

// Program

static Mode mode = Mode::SELECT;
static std::vector<std::unique_ptr<Frame>> frames;
static unsigned frame = 0;
static SDL_FPoint mouse;

/// Mode data (if haskell could be in type :( sad)

// SELECT
static std::unordered_set<RPoint*> selectedPoints;

// ADD
static std::optional<SDL_FPoint> firstPoint;
static std::shared_ptr<RPoint> selectedPoint;

// ROTATE
static const RPoint* pivot;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
	if (!SDL_CreateWindowAndRenderer("shldnbththrd",
					 800, 600,
					 0,
					 &window, &renderer)) {
		SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	const std::deque<std::shared_ptr<RPoint>> empty = {};
	frames.push_back(std::make_unique<Frame>(empty));
	SDL_SetRenderVSync(renderer, 1);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
	constexpr auto selectPoint = [&]
		(this const auto &self,
		 std::multimap<int, std::shared_ptr<RPoint>>& distances,
		 std::unordered_set<RPoint*>& visited, std::shared_ptr<RPoint> p)
		-> void {
		if (visited.contains(p.get()))
			return;
		distances.insert({Distance(p->p, mouse), p});
		for (auto& c : p->c)
			self(distances, visited, c);
	};

	constexpr auto selectAll = [&]
		(this const auto &self,
		 std::unordered_set<RPoint*>& visited, RPoint* p)
		-> void {
		if (visited.contains(p))
			return;
		selectedPoints.insert(p);
		for (auto& c : p->c)
			self(visited, c.get());
	};

	if (event->type == SDL_EVENT_KEY_DOWN) {
		switch (event->key.key)
		{
		case SDLK_1:
			mode = Mode::SELECT;
			selectedPoints.clear();
			pivot = nullptr;
			break;
		case SDLK_2:
			mode = Mode::ADD;
			firstPoint = std::nullopt;
			break;
		case SDLK_3:
			mode = Mode::MOVE;
			break;
		case SDLK_4:
			if (pivot)
				mode = Mode::ROTATE;
			break;
		case SDLK_A: {
			if (mode == Mode::SELECT) {
				std::unordered_set<RPoint*> emptyVisited;
				for (auto p : selectedPoints)
					selectAll(emptyVisited, p);
			}
			break;
		}
		case SDLK_Q: {
			frame = std::max(static_cast<int>(frame-1), 0);
			selectedPoints.clear();
			selectedPoint = nullptr;
			break;
		}
		case SDLK_E: {
			frame++;
			if (frame == frames.size()) {
				const Frame nFrame = *frames.back();
				// Copy
				frames.push_back(
					std::make_unique<Frame>(nFrame));
			}
			selectedPoints.clear();
			selectedPoint = nullptr;
			break;
		}
		case SDLK_D: {
			if (mode == Mode::SELECT) {
				// Probably safe because unordered_sets are unordered?
				for (auto& p : selectedPoints) {
					if (auto par = p->parent) {
						for (auto it = par->c.begin();
						     it != par->c.end(); it++)
							if (it->get() == p) {
								par->c.erase(
									it);
								break;
							}
					} else {
						auto& f = frames.at(frame)
							->points;
						for (auto it = f.begin();
						     it != f.end(); it++)
							if (it->get() == p) {
								f.erase(it);
								break;
							}
					}
				}
				selectedPoints.clear();
			}
			break;
		}
		case SDLK_S:
			ExportAnimation();
			break;
		}
	} else if (event->type == SDL_EVENT_MOUSE_MOTION) {
		mouse = {event->motion.x, event->motion.y };
		switch (mode) {
		case Mode::MOVE: {
			for (auto p : selectedPoints) {
				p->p.x += event->motion.xrel;
				p->p.y += event->motion.yrel;
			}
			break;
		}
		case Mode::ROTATE: {
			const float r = event->motion.yrel / 10;
			for (auto& p : selectedPoints) {
				const float x = p->p.x;
				const float y = p->p.y;
				const float cx = pivot->p.x;
				const float cy = pivot->p.y;
				p->p.x = ((x - cx) * std::cos(r))
					- ((y - cy) * std::sin(r))
					+ cx;
				p->p.y = ((x - cx) * std::sin(r))
					+ ((y - cy) * std::cos(r))
					+ cy;
			}
			break;
		}
		}

	} else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ) {
		switch (mode)
		{
		case Mode::SELECT: {
			std::unordered_set<RPoint*> emptyVisited;
			std::multimap<int, std::shared_ptr<RPoint>> distances;
			for (auto& point: frames.at(frame)->points)
				selectPoint(distances, emptyVisited,
					    point);
			for (auto it = distances.begin();
			     it != distances.end(); it++) {
				if (it->first < 25) {
					if (event->button.button == SDL_BUTTON_LEFT) {
						if (selectedPoints
						    .contains(it->second.get()))
							continue;
						selectedPoints.insert(it->second
								      .get());
					} else
						pivot = it->second.get();

					break;
				} else break;
			}
			break;
		}
		case Mode::ADD: {
			std::unordered_set<RPoint*> emptyVisited;
			std::multimap<int, std::shared_ptr<RPoint>> distances;
			for (auto& point: frames.at(frame)->points)
				selectPoint(distances, emptyVisited,
					    point);
			const std::deque<std::shared_ptr<RPoint>> noChildren = {};

			if (distances.begin() != distances.end() &&
			    distances.begin()->first < 25) {
				if (firstPoint) {
					distances.begin()->second->c.push_back(
						std::make_shared<RPoint>(
						noChildren, firstPoint.value(),
						distances.begin()->second.get()
						));

					selectedPoint = nullptr;
					firstPoint = std::nullopt;
				} else if (selectedPoint) {
					distances.begin()->second->c.push_back(
						selectedPoint
						);
					selectedPoint = nullptr;
				} else
					selectedPoint = distances.begin()->second;

			} else {
				if (firstPoint) {
					const std::deque<std::shared_ptr<RPoint>> c = {
						std::make_shared<RPoint>(
							noChildren,
							mouse, nullptr)};

					frames.at(frame)->points.push_back(
						std::make_shared<RPoint>(
							c,
							firstPoint.value(),
							nullptr));
					selectedPoint = nullptr;
					firstPoint = std::nullopt;
				} else if (selectedPoint) {
					selectedPoint->c.push_back(
						std::make_shared<RPoint>(
							noChildren,
							mouse,
							selectedPoint.get()));
					selectedPoint = nullptr;
				} else
					firstPoint = mouse;
			}
			break;
		}
		case Mode::ROTATE: {
			break;
		}
		}
	} else if (event->type == SDL_EVENT_QUIT) {
		return SDL_APP_SUCCESS;
	}
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
	SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
	SDL_RenderClear(renderer);

	// Mode line
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderDebugTextFormat(renderer, 0, 0,
				  "%s mode - Frame: %d / %zu",
				  modeNames.at(mode).c_str(),
				  frame,
				  frames.size() - 1
		);

	// Onion skins
	for (int i = std::max(static_cast<int>(frame - 3), 0);
	     i < frame; i++) {
		SDL_SetRenderDrawColor(renderer, 170, 100, 0,
				       (3-(frame - i))*70
			);
		for (auto const& point : frames.at(i)->points)
			drawLine(*point);
	}
	for (int i = std::min(static_cast<int>(frame + 3),
			      static_cast<int>(frames.size()) - 1);
	     i > frame; i--) {
		SDL_SetRenderDrawColor(renderer, 0, 100, 170,
				       (3-(i - frame))*70
			);
		for (auto const& point : frames.at(i)->points)
			drawLine(*point);
	}
	// Actual frame
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	for (auto const& point : frames.at(frame)->points)
		drawLine(*point);

	// Selected
	SDL_SetRenderDrawColor(renderer, 255, 0, 0, 100);
	for (const auto& p : selectedPoints) {
		const SDL_FRect rect = {
			p->p.x - 25, p->p.y - 25,
			50, 50
		};
		SDL_RenderFillRect(renderer, &rect);
	}
	if (pivot) {
		SDL_SetRenderDrawColor(renderer, 170, 100, 0, 100);
		const SDL_FRect rect = {
			pivot->p.x - 25, pivot->p.y - 25,
			50, 50
		};
		SDL_RenderFillRect(renderer, &rect);
	}

	SDL_RenderPresent(renderer);
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{}

void drawLine(const RPoint& p) {
	for (auto& c : p.c) {
		SDL_RenderLine(renderer,
			       p.p.x, p.p.y,
			       c->p.x, c->p.y);
		drawLine(*c);
	}
};

void ExportAnimation()
{
	std::vector<int> lens;
	for (int i = 0; i < frames.size(); i++) { lens.push_back(100); };

	std::vector<SDL_Surface*> renderedFrames;

	// Init temporary buffer
	SDL_Texture* temp = SDL_CreateTexture(renderer,
					   SDL_PIXELFORMAT_RGBA8888,
					   SDL_TEXTUREACCESS_TARGET,
					   800, 600);
	SDL_SetRenderTarget(renderer, temp);

	// Draw frames
	for (const auto& f : frames) {
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
		SDL_RenderClear(renderer);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

		for (auto const& point : f->points)
			drawLine(*point);
		renderedFrames.push_back(SDL_RenderReadPixels(renderer, NULL));
	}
	SDL_DestroyTexture(temp);

	// Export

	IMG_Animation vid = {
		800, 600,
		static_cast<int>(frames.size()),
		renderedFrames.data(),
		lens.data()
	};
	IMG_SaveAnimation(&vid, "./out.gif");

	// Cleanup
	for (auto& s : renderedFrames)
		SDL_DestroySurface(s);
}

constexpr float Distance(SDL_FPoint p1, SDL_FPoint p2)
{
	return std::sqrt(
		std::pow(p2.x - p1.x, 2) + std::pow(p2.y - p1.y, 2));
}
