import { mount } from 'svelte';
import './app.css';
import App from './App.svelte';
import { prefs } from './stores/prefs.svelte';

const target = document.getElementById('app');
if (!target) throw new Error('missing #app');

// The person's theme, language and text size go on <html> before the first paint.
prefs.apply();

export default mount(App, { target });
