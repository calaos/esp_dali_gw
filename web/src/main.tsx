import { render } from 'preact';

import { App } from './app.tsx';
import './styles.css';

const root = document.getElementById('app');
if (!root) {
    throw new Error('#app container missing from index.html');
}
render(<App />, root);
