# TRSR website

Static front end intended for `trsr.app`. The Cloudflare Worker at `api.trsr.app` supplies results, activity, and browser account sessions.

## Pages

- Home: client download and current queue/race activity.
- Leaderboards: all 15 Anniversary levels, both categories, and all four subcategories. Filters request matching results from the Worker.
- Players: current queue members and recent race participants.
- Profile: sign in with a Speedrun.com API key to see completed runs and winning finishes.
- Download and About.

Croft Manor is included in both the website filters and the Worker's matchmaking level list.

## Public API

- `GET /public/overview` returns queue, race, and active player counts.
- `GET /public/players` returns queue members and recent race participants.
- `GET /public/leaderboards?game=anniversary` accepts optional `level`, `category`, and `subcategory` filters. Results are grouped by player and ruleset, ranked by best winning in-game time. Forfeit wins have no completion time and do not appear.

The all-level view returns up to ten places per ruleset. Choosing a level returns up to 100 places per ruleset.

## Web sign-in

The website sends the Speedrun.com API key to `POST /auth/speedrun` once over HTTPS. For requests from `https://trsr.app`, the Worker returns no session token in the response body and sets an HttpOnly, Secure, SameSite=Lax cookie on `api.trsr.app`. The key is not stored. The desktop client continues to use the token response without a browser `Origin` header.

`GET /auth/me` returns the profile and stats for the cookie session. `POST /auth/logout` removes the session and clears the cookie.

## Before deployment

Apply the existing D1 schema and migrations through `0007`, then deploy the updated Worker before publishing the static site. Review the production database for prototype test matches before exposing the leaderboard. These routes have been checked locally for syntax and filter handling but have not been tested against production D1.

The active player count reflects players in the queue or with recent match presence; it does not include idle clients. A full online player list needs a separate client heartbeat.

## Local preview

Open `index.html` to review the layout. A `file:` preview may not reach the live API because the Worker only allows the `https://trsr.app` origin. The page shows unavailable states when API requests fail. There are no build dependencies.
