#include "SDL3/SDL_stdinc.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#define SDL_MAIN_USE_CALLBACKS 1
#include "SDL3/SDL_error.h"
#include "SDL3/SDL_iostream.h"
#include "SDL3/SDL_pixels.h"
#include <SDL3/SDL_surface.h>
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

#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <cmath>

enum class Mode
{
	SELECT,
	ADD,
	MOVE,
	ROTATE,
};

enum class LineType
{
	LINE,
	CIRCLE,
};

struct RPoint;

using DrawnLine = std::pair<std::shared_ptr<RPoint>, LineType>;
using Line = std::pair<const RPoint*, const RPoint*>;

namespace std {
template <> struct hash<std::pair<const RPoint*, const RPoint*>> {
    inline size_t operator()(const std::pair<const RPoint*, const RPoint*> &v) const {
        std::hash<const RPoint*> ptr_hasher;
        return ptr_hasher(v.first) ^ ptr_hasher(v.second);
    }
};
}

struct RPoint
{
	std::vector<DrawnLine> c;
	SDL_FPoint p;
	RPoint* parent;

	RPoint() : c({}), p({0,0}), parent(nullptr) {};

	RPoint(std::vector<DrawnLine> c,
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
			for (const auto [p, lt] :
				     target.c) {
				if (map.contains(p.get())) {
					point->c.push_back(
						{map.at(p.get()),lt});
				} else {
					auto neww =
						std::make_shared<RPoint>();
					map.insert({p.get(), neww});
					cp(neww.get(), *p);
					point->c.push_back({neww,lt});
					point->c.back().first->parent = point;
				}
			}
		};
		cp(this, obj);
	}
	// Probably not needed?
	RPoint (RPoint&& other) = delete;
};

struct Frame {
	std::vector<std::shared_ptr<RPoint>> points;

	Frame(std::vector<std::shared_ptr<RPoint>> points) : points(points) {}
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

static const std::unordered_map<Mode, std::string> modeNames = {
	{ Mode::SELECT,	"Select"},
	{ Mode::ADD,	"Add"},
	{ Mode::MOVE,	"Move"},
	{ Mode::ROTATE,	"Rotate"},
};

static const std::unordered_map<LineType, std::string> lineNames = {
	{ LineType::LINE,	"lines"},
	{ LineType::CIRCLE,	"circles"},
};

void drawLine(const RPoint* p);
void ExportAnimation();
void LoadSVG(SDL_Texture*& dst, const std::string& svg, int w, int h);
size_t countLines(RPoint* p, std::unordered_set<RPoint*>& visited);
constexpr float Distance(SDL_FPoint p1, SDL_FPoint p2);

// SDL

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

// Program

static Mode mode = Mode::SELECT;
// Could use list but I don't how to store references
// to a particular position sooooo vector it is
static std::vector<std::unique_ptr<Frame>> frames;
static unsigned frame = 0;
static SDL_FPoint mouse;
static SDL_Texture* circleTexture = nullptr;

/// Mode data (if haskell could be in type :( sad)

// SELECT
static std::unordered_set<RPoint*> selectedPoints;

// ADD
static std::optional<SDL_FPoint> firstPoint;
static std::shared_ptr<RPoint> selectedPoint;
static LineType lineType;

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
	SDL_SetRenderVSync(renderer, 1);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

	const std::string circleSVG =
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10mm\" height=\"10mm\" viewBox=\"0 0 10 10\"><circle cx=\"5\" cy=\"5\" r=\"5\" style=\"fill:#ffffff;stroke-width:.417999;stroke-linecap:round;stroke-linejoin:round;fill-opacity:1\"/></svg>";
	const std::string moi =
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10mm\" height=\"10mm\" viewBox=\"0 0 10 10\"><path d=\"M-10.068 3.92h8.518v1.84h-8.518z\" style=\"fill:#000;stroke-width:.560049;stroke-linecap:round;stroke-linejoin:round\" transform=\"rotate(-90)\"/><path d=\"m2.228 3.301 5.312.075L4.958.126Z\" style=\"fill:#000;stroke-width:.560049;stroke-linecap:round;stroke-linejoin:round\"/></svg>";

	LoadSVG(circleTexture, circleSVG, 500, 500);

	const std::vector<std::shared_ptr<RPoint>> empty = {};
	frames.push_back(std::make_unique<Frame>(	empty));


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
		visited.insert(p.get());
		distances.insert({Distance(p->p, mouse), p});
		for (auto& [c, _] : p->c)
			self(distances, visited, c);
	};

	constexpr auto selectAll = [&]
		(this const auto &self,
		 std::unordered_set<RPoint*>& visited, RPoint* p)
		-> void {
		if (visited.contains(p))
			return;
		visited.insert(p);
		selectedPoints.insert(p);
		for (auto& [c, _] : p->c)
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
		case SDLK_Z:
			lineType = LineType::LINE;
			break;
		case SDLK_X:
			lineType = LineType::CIRCLE;
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
							if (it->first.get() == p) {
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
		case SDLK_K: {
			if (frames.size() == 1) {
				frames.at(frame)->points.clear();
			} else {
				frames.erase(frames.begin() + frame);
				frame = std::max(static_cast<int>(frame - 1), 0);
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
			const std::vector<DrawnLine> noChildren = {};

			if (distances.begin() != distances.end() &&
			    distances.begin()->first < 25) {
				if (firstPoint) {
					distances.begin()->second->c.push_back({
							std::make_shared<RPoint>(
								noChildren,
								firstPoint.value(),
								distances.begin()->
								second.get()
								), lineType});

					selectedPoint = nullptr;
					firstPoint = std::nullopt;
				} else if (selectedPoint) {
					distances.begin()->second->c.push_back(
						{selectedPoint, lineType});
					selectedPoint = nullptr;
				} else
					selectedPoint = distances.begin()->second;

			} else {
				if (firstPoint) {
					const std::vector<DrawnLine> c
						= { {std::make_shared<RPoint>(
								noChildren,
								mouse, nullptr),
							     lineType}};

					frames.at(frame)->points.push_back(
						std::make_shared<RPoint>(
							c,
							firstPoint.value(),
							nullptr));
					selectedPoint = nullptr;
					firstPoint = std::nullopt;
				} else if (selectedPoint) {
					selectedPoint->c.push_back({
							std::make_shared<RPoint>(
								noChildren,
								mouse,
								selectedPoint.get()),
							lineType});
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
	size_t sum = 0;
	{
		std::unordered_set<RPoint*> visited;
		for (const auto& p : frames.at(frame)->points) {
			sum += countLines(p.get(), visited);
		}
	}
	SDL_RenderDebugTextFormat(renderer, 0, 0,
				  "%s mode - Drawing %s - Frame: %d / %zu"
				  " - Lines: %zu",
				  modeNames.at(mode).c_str(),
				  lineNames.at(lineType).c_str(),
				  frame,
				  frames.size() - 1,
				  sum
		);

	// Onion skins
	for (int i = std::max(static_cast<int>(frame - 3), 0);
	     i < frame; i++) {
		const uint8_t a = (3-(i - frame))*70;
		SDL_SetRenderDrawColor(renderer, 170, 100, 0, a);
		SDL_SetTextureColorMod(circleTexture, 0, 100, 170);
		SDL_SetTextureAlphaMod(circleTexture, a);
		for (auto const& point : frames.at(i)->points)
			drawLine(point.get());
	}
	for (int i = std::min(static_cast<int>(frame + 3),
			      static_cast<int>(frames.size()) - 1);
	     i > frame; i--) {
		const uint8_t a = (3-(i - frame))*70;
		SDL_SetRenderDrawColor(renderer, 0, 100, 170, a);
		SDL_SetTextureColorMod(circleTexture, 0, 100, 170);
		SDL_SetTextureAlphaMod(circleTexture, a);
		for (auto const& point : frames.at(i)->points)
			drawLine(point.get());
	}
	// Actual frame
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_SetTextureColorMod(circleTexture, 0, 0, 0);
	SDL_SetTextureAlphaMod(circleTexture, 255);
	for (auto const& point : frames.at(frame)->points)
		drawLine(point.get());

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

void drawLine_(const RPoint* p, std::unordered_set<Line>& visited)
{
	for (auto& [c, lt] : p->c) {
		const Line pair = {p, c.get()};
		if (visited.contains(pair))
			continue;
		visited.insert(pair);

		switch (lt)
		{
		case LineType::LINE:
			SDL_RenderLine(renderer,
				       p->p.x, p->p.y,
				       c->p.x, c->p.y);
			break;
		case LineType::CIRCLE: {
			// 7th grade level maths :shout: :fire:
			const float w = c->p.x - p->p.x;
			const float h = p->p.y - c->p.y;
			const float hyp = std::sqrt(std::pow(h, 2)
						    + std::pow(w, 2));
			const SDL_FRect dstrect = {
				p->p.x - (hyp/2), p->p.y-hyp,
				hyp, hyp
			};

			const double angle = std::asin(h/hyp) * (180/M_PI);
			const double rangle = (w < 0) ? (angle-90)
				: (-angle+90);
			const SDL_FPoint center = {dstrect.w/2,dstrect.h};

			SDL_RenderTextureRotated(renderer, circleTexture,
						 nullptr, &dstrect,
						 rangle, &center,
						 SDL_FLIP_NONE);
			break;
		}
		}
		drawLine_(c.get(), visited);
	}
};

void drawLine(const RPoint* p)
{
	std::unordered_set<Line> visited;
	drawLine_(p, visited);
}

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
			drawLine(point.get());
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

void LoadSVG(SDL_Texture*& dst, const std::string& svg, int w, int h)
{
	SDL_IOStream* io = SDL_IOFromConstMem(svg.data(), svg.size());
	SDL_Surface* tmp = IMG_LoadSizedSVG_IO(io, w, h);
	dst = SDL_CreateTextureFromSurface(renderer, tmp);
	SDL_DestroySurface(tmp);
}

size_t countLines(RPoint* p, std::unordered_set<RPoint*>& visited)
{
	if (visited.contains(p))
		return 1;
	visited.insert(p);
	std::vector<size_t> sizes;
	for (const auto& c : p->c) {
		sizes.push_back(countLines(c.first.get(), visited));
	}
	return std::accumulate(sizes.begin(), sizes.end(), 1);
}

constexpr float Distance(SDL_FPoint p1, SDL_FPoint p2)
{
	return std::sqrt(
		std::pow(p2.x - p1.x, 2) + std::pow(p2.y - p1.y, 2));
}
